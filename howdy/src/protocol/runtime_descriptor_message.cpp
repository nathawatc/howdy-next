#include "protocol/runtime_descriptor_message.hpp"

#include "support/bit_flags.hpp"
#include "support/eintr.hpp"
#include "support/scoped_fd.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

#include <unistd.h>

#include <sys/socket.h>
#include <sys/uio.h>  // IWYU pragma: keep

namespace howdy::native::runtime_descriptor_message {

	namespace {

		// Room for more descriptors than the protocol allows plus credentials, so a sender that
		// adds anything shows up as an invalid header rather than only as truncation.
		constexpr std::size_t kControlCapacity =
		    CMSG_SPACE(sizeof(int) * (kMaxDescriptors + 2)) + CMSG_SPACE(sizeof(ucred));

		struct ControlWalk {
			std::vector<int> descriptors;
			std::size_t      rights_headers = 0;
			bool             other_headers  = false;
			bool             malformed      = false;
		};

		// Walks control bytes that came from anywhere: every header length is checked against
		// what is left before any of its data is read.
		auto WalkControl(std::span<const std::byte> control) -> ControlWalk {
			ControlWalk                                              walk;
			alignas(cmsghdr) std::array<std::byte, kControlCapacity> buffer{};
			if (control.size() > buffer.size()) {
				walk.malformed = true;
				return walk;
			}
			std::ranges::copy(control, buffer.begin());
			msghdr message{};
			message.msg_control    = buffer.data();
			message.msg_controllen = control.size();
			for (cmsghdr *header = CMSG_FIRSTHDR(&message); header != nullptr;
			     header          = CMSG_NXTHDR(&message, header)) {
				const std::size_t offset = std::bit_cast<std::uintptr_t>(header) -
				                           std::bit_cast<std::uintptr_t>(buffer.data());
				if (header->cmsg_len < CMSG_LEN(0) ||
				    header->cmsg_len > message.msg_controllen - offset) {
					walk.malformed = true;
					return walk;
				}
				if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS) {
					walk.other_headers = true;
					continue;
				}
				++walk.rights_headers;
				const std::size_t data_size = header->cmsg_len - CMSG_LEN(0);
				if (data_size % sizeof(int) != 0) {
					walk.malformed = true;
				}
				auto data = std::span<const unsigned char>(CMSG_DATA(header), data_size);
				while (data.size() >= sizeof(int)) {
					int descriptor = -1;
					std::memcpy(&descriptor, data.data(), sizeof(descriptor));
					walk.descriptors.push_back(descriptor);
					data = data.subspan(sizeof(int));
				}
			}
			return walk;
		}

	}  // namespace

	auto DecodeMessage(const RawMessage &raw) -> DecodedMessage {
		auto           walk = WalkControl(raw.control);
		DecodedMessage decoded{.descriptors = std::move(walk.descriptors)};
		if (HasAnyBits(raw.flags, MSG_TRUNC, MSG_CTRUNC) || walk.malformed || walk.other_headers ||
		    walk.rights_headers != 1 || raw.payload.size() != kPayloadSize ||
		    std::to_integer<char>(raw.payload.front()) != kMarker) {
			return decoded;
		}
		const auto state = ParseModelState(std::to_integer<char>(raw.payload.back()));
		if (!state.has_value() || decoded.descriptors.size() != ExpectedDescriptorCount(*state)) {
			return decoded;
		}
		decoded.valid       = true;
		decoded.model_state = *state;
		return decoded;
	}

	auto SendMessage(int socket_fd, const OutgoingMessage &outgoing) -> bool {
		const bool with_model = outgoing.model_state == ModelState::kReady;
		if (outgoing.config_fd < 0 || with_model != (outgoing.model_fd >= 0)) {
			return false;
		}
		std::array<char, kPayloadSize> payload{kMarker, static_cast<char>(outgoing.model_state)};
		iovec vector{.iov_base = payload.data(), .iov_len = payload.size()};
		const std::array<int, kMaxDescriptors> descriptors{outgoing.config_fd, outgoing.model_fd};
		const std::size_t                      descriptor_bytes =
		    sizeof(int) * ExpectedDescriptorCount(outgoing.model_state);
		alignas(cmsghdr) std::array<std::byte, CMSG_SPACE(sizeof(int) * kMaxDescriptors)> control{};
		msghdr                                                                            message{};
		message.msg_iov        = &vector;
		message.msg_iovlen     = 1;
		message.msg_control    = control.data();
		message.msg_controllen = CMSG_SPACE(descriptor_bytes);
		cmsghdr *header        = CMSG_FIRSTHDR(&message);
		header->cmsg_level     = SOL_SOCKET;
		header->cmsg_type      = SCM_RIGHTS;
		header->cmsg_len       = CMSG_LEN(descriptor_bytes);
		std::memcpy(CMSG_DATA(header), descriptors.data(), descriptor_bytes);
		return RetryOnEintr([&] -> ssize_t {
			       return sendmsg(socket_fd, &message, MSG_NOSIGNAL);
		       }) == static_cast<ssize_t>(payload.size());
	}

	auto ReceiveMessage(int socket_fd) -> ReceivedMessage {
		std::array<std::byte, kPayloadSize + 1>                  payload{};
		alignas(cmsghdr) std::array<std::byte, kControlCapacity> control{};
		iovec  vector{.iov_base = payload.data(), .iov_len = payload.size()};
		msghdr message{};
		message.msg_iov        = &vector;
		message.msg_iovlen     = 1;
		message.msg_control    = control.data();
		message.msg_controllen = control.size();
		const ssize_t received = recvmsg(socket_fd, &message, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
		if (received < 0) {
			const int error_number = errno;
			if (error_number == EAGAIN || error_number == EWOULDBLOCK || error_number == EINTR) {
				return {.status = ReceiveStatus::kRetry};
			}
			return {.status = ReceiveStatus::kError, .error_number = error_number};
		}

		const auto decoded = DecodeMessage({
		    .payload =
		        std::span<const std::byte>(payload).first(static_cast<std::size_t>(received)),
		    .control = std::span<const std::byte>(control).first(message.msg_controllen),
		    .flags   = message.msg_flags,
		});
		// Owned at once, so every descriptor the kernel installed is closed on any rejection.
		std::vector<howdy::native::ScopedFd> owned;
		owned.reserve(decoded.descriptors.size());
		for (const int descriptor : decoded.descriptors) {
			owned.emplace_back(descriptor);
		}
		if (received == 0 && message.msg_controllen == 0) {
			return {.status = ReceiveStatus::kEndOfFile};
		}
		if (!decoded.valid) {
			return {.status = ReceiveStatus::kInvalid};
		}
		ReceivedMessage result{
		    .status      = ReceiveStatus::kMessage,
		    .model_state = decoded.model_state,
		    .config      = std::move(owned.at(0)),
		};
		if (owned.size() > 1) {
			result.model = std::move(owned.at(1));
		}
		return result;
	}

}  // namespace howdy::native::runtime_descriptor_message
