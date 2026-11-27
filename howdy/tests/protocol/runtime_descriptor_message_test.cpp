#include "protocol/runtime_descriptor_message.hpp"
#include "support/bit_flags.hpp"
#include "support/scoped_fd.hpp"
#include "test_support.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/uio.h>  // IWYU pragma: keep
#include <sys/wait.h>

namespace {

	namespace fs      = std::filesystem;
	namespace message = howdy::native::runtime_descriptor_message;
	using howdy::native::ScopedFd;
	using howdy::test::Expect;
	using message::ModelState;
	using message::ReceiveStatus;

	struct SocketPair {
		ScopedFd sender;
		ScopedFd receiver;
	};

	auto MakeSocketPair() -> SocketPair {
		std::array<int, 2> fds{-1, -1};
		if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, fds.data()) != 0) {
			return {};
		}
		return {.sender = ScopedFd(fds.at(0)), .receiver = ScopedFd(fds.at(1))};
	}

	auto OpenDescriptorCount() -> std::size_t {
		std::size_t     count = 0;
		std::error_code error;
		for (const auto &entry : fs::directory_iterator("/proc/self/fd", error)) {
			(void)entry;
			++count;
		}
		return count;
	}

	// Sends a message by hand, so the tests can break the format in ways SendMessage refuses to.
	struct RawSend {
		std::string                   payload;
		std::vector<std::vector<int>> rights;  // one SCM_RIGHTS header per entry
		// The receiver asks for credentials, so the kernel adds an SCM_CREDENTIALS header.
		bool credentials = false;
	};

	auto SendRaw(int socket_fd, const RawSend &raw) -> bool {
		std::string payload = raw.payload;
		iovec       vector{.iov_base = payload.data(), .iov_len = payload.size()};
		alignas(cmsghdr) std::array<std::byte, 256> control{};
		msghdr                                      message{};
		message.msg_iov        = &vector;
		message.msg_iovlen     = 1;
		message.msg_control    = control.data();
		message.msg_controllen = control.size();
		std::size_t used       = 0;
		cmsghdr    *header     = CMSG_FIRSTHDR(&message);
		for (const auto &descriptors : raw.rights) {
			const std::size_t bytes = sizeof(int) * descriptors.size();
			header->cmsg_level      = SOL_SOCKET;
			header->cmsg_type       = SCM_RIGHTS;
			header->cmsg_len        = CMSG_LEN(bytes);
			std::memcpy(CMSG_DATA(header), descriptors.data(), bytes);
			used += CMSG_SPACE(bytes);
			header = CMSG_NXTHDR(&message, header);
		}
		message.msg_controllen = used;
		if (used == 0) {
			message.msg_control = nullptr;
		}
		return sendmsg(socket_fd, &message, MSG_NOSIGNAL) == static_cast<ssize_t>(payload.size());
	}

	auto CloseOnExec(int fd) -> bool {
		const int flags = fcntl(fd, F_GETFD);
		return flags >= 0 && howdy::native::HasAnyBits(flags, FD_CLOEXEC);
	}

	auto TestRoundTrips(const ScopedFd &config, const ScopedFd &model) -> bool {
		bool ok = true;
		for (const auto state : {
		         ModelState::kReady,
		         ModelState::kMissing,
		         ModelState::kInvalidUser,
		         ModelState::kInvalidStorage,
		     }) {
			auto       pair       = MakeSocketPair();
			const bool with_model = state == ModelState::kReady;
			ok &= Expect(message::SendMessage(pair.sender.Get(),
			                                  {
			                                      .model_state = state,
			                                      .config_fd   = config.Get(),
			                                      .model_fd    = with_model ? model.Get() : -1,
			                                  }),
			             "send a descriptor message");
			pair.sender.Reset();
			const auto received = message::ReceiveMessage(pair.receiver.Get());
			ok &= Expect(received.status == ReceiveStatus::kMessage &&
			                 received.model_state == state && received.config.Valid() &&
			                 CloseOnExec(received.config.Get()) &&
			                 received.model.Valid() == with_model &&
			                 (!with_model || CloseOnExec(received.model.Get())),
			             "receive the message with close-on-exec descriptors");
			char byte = 0;
			ok &= Expect(pread(received.config.Get(), &byte, 1, 0) == 1 && byte == 'c',
			             "the received config descriptor reads the config");
			ok &= Expect(message::ReceiveMessage(pair.receiver.Get()).status ==
			                 ReceiveStatus::kEndOfFile,
			             "end of file follows the message");
		}
		return ok;
	}

	auto TestSendRefusesBrokenMessages(const ScopedFd &config, const ScopedFd &model) -> bool {
		const auto pair = MakeSocketPair();
		const auto send = [&](message::OutgoingMessage outgoing) -> bool {
			return message::SendMessage(pair.sender.Get(), outgoing);
		};
		bool ok = Expect(!send({
		                     .model_state = ModelState::kReady,
		                     .config_fd   = config.Get(),
		                 }),
		                 "send refuses a ready state without a model");
		ok &= Expect(!send({
		                 .model_state = ModelState::kMissing,
		                 .config_fd   = config.Get(),
		                 .model_fd    = model.Get(),
		             }),
		             "send refuses a model with a non-ready state");
		ok &= Expect(!send({
		                 .model_state = ModelState::kMissing,
		             }),
		             "send refuses a message without a config");
		ok &= Expect(message::ReceiveMessage(pair.receiver.Get()).status == ReceiveStatus::kRetry,
		             "a refused send sends nothing");
		return ok;
	}

	auto TestRejections(const ScopedFd &config, const ScopedFd &model) -> bool {
		bool       ok     = true;
		const int  cfg    = config.Get();
		const int  mdl    = model.Get();
		const auto reject = [&](const RawSend &raw, const char *label) -> bool {
			const auto pair = MakeSocketPair();
			if (raw.credentials) {
				const int enable = 1;
				(void)setsockopt(pair.receiver.Get(), SOL_SOCKET, SO_PASSCRED, &enable,
				                 sizeof(enable));
			}
			const std::size_t before = OpenDescriptorCount();
			bool              local  = Expect(SendRaw(pair.sender.Get(), raw), label);
			const auto        status = message::ReceiveMessage(pair.receiver.Get()).status;
			local &= Expect(status == ReceiveStatus::kInvalid, label);
			local &= Expect(OpenDescriptorCount() == before,
			                std::string(label) + " leaves no received descriptor open");
			return local;
		};
		ok &= reject({.payload = "XR", .rights = {{cfg, mdl}}}, "wrong marker is invalid");
		ok &= reject({.payload = "DZ", .rights = {{cfg}}}, "unknown state is invalid");
		ok &= reject({.payload = "DR", .rights = {{cfg}}}, "ready with one descriptor is invalid");
		ok &= reject({.payload = "DM", .rights = {{cfg, mdl}}},
		             "missing with two descriptors is invalid");
		ok &= reject({.payload = "D", .rights = {{cfg}}}, "short payload is invalid");
		ok &= reject({.payload = "DMX", .rights = {{cfg}}}, "long payload is invalid");
		ok &= reject({.payload = std::string(64, 'D'), .rights = {{cfg}}},
		             "truncated payload is invalid");
		ok &= reject({.payload = "DM", .rights = {}}, "no descriptors is invalid");
		ok &= reject({.payload = "DM", .rights = {{cfg, cfg, cfg, cfg, cfg, cfg, cfg}}},
		             "too many descriptors are invalid");
		ok &= reject({.payload = "DM", .rights = {{cfg}}, .credentials = true},
		             "credentials next to the descriptors are invalid");
		ok &= reject({.payload = "", .rights = {{cfg}}},
		             "zero-byte packet with descriptors is invalid");
		return ok;
	}

	auto ExhaustDescriptorsAndReceive(int receiver_fd) -> int {
		rlimit limit{};
		if (getrlimit(RLIMIT_NOFILE, &limit) != 0) {
			return EXIT_FAILURE;
		}
		const rlim_t target_cur = static_cast<rlim_t>(receiver_fd) + 1;
		if (limit.rlim_cur > target_cur) {
			limit.rlim_cur = target_cur;
			if (setrlimit(RLIMIT_NOFILE, &limit) != 0) {
				return EXIT_FAILURE;
			}
		}
		std::vector<ScopedFd> fillers;
		while (true) {
			const int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
			if (fd < 0) {
				if (errno != EMFILE) {
					return EXIT_FAILURE;
				}
				break;
			}
			fillers.emplace_back(fd);
		}
		const auto received = message::ReceiveMessage(receiver_fd);
		return received.status == ReceiveStatus::kInvalid ? EXIT_SUCCESS : EXIT_FAILURE;
	}

	// Exhausts the subprocess descriptor table so recvmsg drops incoming SCM_RIGHTS with
	// MSG_CTRUNC and msg_controllen == 0; a 0-byte packet must still classify as kInvalid, not EOF.
	auto TestTruncatedZeroBytePacketIsInvalid(const ScopedFd &config) -> bool {
		bool ok = true;
		for (const auto &[payload, label] : {
		         std::pair{std::string{}, "zero-byte packet with MSG_CTRUNC is invalid, not EOF"},
		         std::pair{std::string("DM"),
				           "truncated SCM_RIGHTS packet under EMFILE is invalid"},
		     }) {
			auto pair = MakeSocketPair();
			if (!Expect(pair.sender.Valid() && pair.receiver.Valid() &&
			                SendRaw(pair.sender.Get(),
			                        {.payload = payload, .rights = {{config.Get()}}}),
			            std::string("queue ") + label)) {
				return false;
			}
			pair.sender.Reset();
			const pid_t child = fork();
			if (child == 0) {
				_exit(ExhaustDescriptorsAndReceive(pair.receiver.Get()));
			}
			int status = 0;
			ok &= Expect(child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) &&
			                 WEXITSTATUS(status) == EXIT_SUCCESS,
			             label);
		}
		return ok;
	}

	auto TestEmptyAndClosed() -> bool {
		auto pair = MakeSocketPair();
		bool ok =
		    Expect(message::ReceiveMessage(pair.receiver.Get()).status == ReceiveStatus::kRetry,
			       "an empty socket asks to retry");
		pair.sender.Reset();
		ok &=
		    Expect(message::ReceiveMessage(pair.receiver.Get()).status == ReceiveStatus::kEndOfFile,
			       "a closed peer is end of file");
		ok &= Expect(message::ReceiveMessage(-1).status == ReceiveStatus::kError,
		             "an invalid socket is an error");
		return ok;
	}

	// Writes one control header and its descriptors into `at`, as the kernel lays them out.
	void PutHeader(std::span<std::byte> at, std::size_t length, std::span<const int> descriptors) {
		cmsghdr header{};
		header.cmsg_len   = length;
		header.cmsg_level = SOL_SOCKET;
		header.cmsg_type  = SCM_RIGHTS;
		std::memcpy(at.data(), &header, sizeof(header));
		std::memcpy(at.subspan(CMSG_LEN(0)).data(), descriptors.data(), descriptors.size_bytes());
	}

	// Decoder cases the kernel never produces, which only the pure decoder can be handed.
	auto TestDecoderBounds() -> bool {
		bool                                       ok = true;
		alignas(cmsghdr) std::array<std::byte, 64> control{};
		const std::array<std::byte, 2>             missing{std::byte{'D'}, std::byte{'M'}};
		const std::array<std::byte, 2>             ready{std::byte{'D'}, std::byte{'R'}};
		const std::array                           seven{7};
		const std::array                           eight{8};
		const auto decode = [&](std::span<const std::byte> payload, std::size_t control_size,
		                        int flags) -> message::DecodedMessage {
			return message::DecodeMessage({
			    .payload = payload,
			    .control = std::span(control).first(control_size),
			    .flags   = flags,
			});
		};

		PutHeader(control, CMSG_LEN(sizeof(int)) + 1000, seven);
		ok &= Expect(!decode(missing, CMSG_SPACE(sizeof(int)), 0).valid,
		             "a header longer than the control data is invalid");
		PutHeader(control, CMSG_LEN(sizeof(int) + 1), seven);
		ok &= Expect(!decode(missing, CMSG_SPACE(sizeof(int) + 1), 0).valid,
		             "descriptor data that is not whole ints is invalid");
		PutHeader(control, 1, seven);
		ok &= Expect(!decode(missing, CMSG_SPACE(sizeof(int)), 0).valid,
		             "a header shorter than itself is invalid");
		ok &= Expect(!decode(missing, 3, 0).valid, "control data shorter than a header is invalid");

		PutHeader(control, CMSG_LEN(sizeof(int)), seven);
		const auto valid = decode(missing, CMSG_SPACE(sizeof(int)), 0);
		ok &= Expect(valid.valid && valid.model_state == ModelState::kMissing &&
		                 valid.descriptors == std::vector<int>{7},
		             "a well-formed message decodes");
		const auto truncated = decode(missing, CMSG_SPACE(sizeof(int)), MSG_CTRUNC);
		ok &= Expect(!truncated.valid && truncated.descriptors == std::vector<int>{7},
		             "a truncated message is invalid but still reports its descriptors");
		for (const int flag : {MSG_CTRUNC, MSG_TRUNC}) {
			const auto empty_truncated = decode({}, 0, flag);
			ok &= Expect(!empty_truncated.valid && empty_truncated.descriptors.empty(),
			             "empty truncated message decodes as invalid");
		}

		// The kernel merges several SCM_RIGHTS headers of one sendmsg into one, so only a
		// decoder handed raw bytes can see two.
		PutHeader(std::span(control).subspan(CMSG_SPACE(sizeof(int))), CMSG_LEN(sizeof(int)),
		          eight);
		const auto doubled = decode(ready, 2 * CMSG_SPACE(sizeof(int)), 0);
		ok &= Expect(!doubled.valid && doubled.descriptors == std::vector<int>{7, 8},
		             "two SCM_RIGHTS headers are invalid but report every descriptor");
		return ok;
	}

}  // namespace

auto main() -> int {
	const auto     config_path = fs::current_path() / "howdy-runtime-descriptor-message-config";
	const auto     model_path  = fs::current_path() / "howdy-runtime-descriptor-message-model";
	bool           ok          = Expect(howdy::test::WriteFile(config_path, "config") &&
	                                        howdy::test::WriteFile(model_path, "model"),
	                                    "write descriptor fixtures");
	const ScopedFd config(open(config_path.c_str(), O_RDONLY | O_CLOEXEC));
	const ScopedFd model(open(model_path.c_str(), O_RDONLY | O_CLOEXEC));
	ok &= Expect(config.Valid() && model.Valid(), "open descriptor fixtures");

	ok &= TestRoundTrips(config, model);
	ok &= TestSendRefusesBrokenMessages(config, model);
	ok &= TestRejections(config, model);
	ok &= TestTruncatedZeroBytePacketIsInvalid(config);
	ok &= TestEmptyAndClosed();
	ok &= TestDecoderBounds();

	std::error_code error;
	fs::remove(config_path, error);
	fs::remove(model_path, error);
	return ok ? 0 : 1;
}
