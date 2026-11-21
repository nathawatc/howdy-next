#pragma once

#include "support/scoped_fd.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace howdy::native::runtime_descriptor_message {

	// The auth helper's whole result is one SOCK_SEQPACKET message on its descriptor socket:
	//
	//   payload  kMarker, then the model state (one byte each, nothing else)
	//   control  exactly one SCM_RIGHTS header carrying [config] or [config, model]
	//
	// The model descriptor is there exactly when the state is kReady. After the message the
	// helper exits, so the socket reaches end of file; PAM requires that and treats anything else
	// (no message, a second message, other control data, a truncated message) as a failure. The
	// helper's exit status is never part of the result. When the config cannot be opened the
	// helper sends nothing. A socket, unlike a pipe, cannot be reopened through /proc/<pid>/fd, so
	// no other process can add a message.
	inline constexpr int              kHelperSocketFd = 3;
	inline constexpr char             kMarker         = 'D';
	inline constexpr std::size_t      kPayloadSize    = 2;
	inline constexpr std::size_t      kMaxDescriptors = 2;
	inline constexpr std::string_view kOpenSubcommand = "open";

	enum class ModelState : char {
		kReady          = 'R',
		kMissing        = 'M',
		kInvalidUser    = 'U',
		kInvalidStorage = 'I',
	};

	[[nodiscard]] constexpr auto ParseModelState(char value) -> std::optional<ModelState> {
		switch (static_cast<ModelState>(value)) {
			case ModelState::kReady:
			case ModelState::kMissing:
			case ModelState::kInvalidUser:
			case ModelState::kInvalidStorage:
				return static_cast<ModelState>(value);
		}
		return std::nullopt;
	}

	[[nodiscard]] constexpr auto ExpectedDescriptorCount(ModelState state) -> std::size_t {
		return state == ModelState::kReady ? 2 : 1;
	}

	struct DecodedMessage {
		bool       valid       = false;
		ModelState model_state = ModelState::kInvalidStorage;
		// Every descriptor found in an SCM_RIGHTS header, in order, also when the message is
		// invalid: the kernel installed each of them, so the receiver must close them all.
		std::vector<int> descriptors;
	};

	// What one recvmsg returned: the payload bytes it read, the control bytes it filled
	// (msg_controllen) and its msg_flags.
	struct RawMessage {
		std::span<const std::byte> payload;
		std::span<const std::byte> control;
		int                        flags = 0;
	};

	// Pure, so it can be fuzzed.
	auto DecodeMessage(const RawMessage &raw) -> DecodedMessage;

	struct OutgoingMessage {
		ModelState model_state = ModelState::kInvalidStorage;
		int        config_fd   = -1;
		// Set exactly when model_state is kReady.
		int model_fd = -1;
	};

	// Sends the one message; false, without sending, when it breaks the format.
	auto SendMessage(int socket_fd, const OutgoingMessage &outgoing) -> bool;

	enum class ReceiveStatus : std::uint8_t {
		kMessage,
		// Nothing to read yet (non-blocking).
		kRetry,
		// The peer closed the socket without a message waiting.
		kEndOfFile,
		// A message arrived but breaks the format; its descriptors are already closed.
		kInvalid,
		kError,
	};

	struct ReceivedMessage {
		ReceiveStatus           status      = ReceiveStatus::kError;
		ModelState              model_state = ModelState::kInvalidStorage;
		howdy::native::ScopedFd config;
		howdy::native::ScopedFd model;
		int                     error_number = 0;
	};

	// One non-blocking recvmsg. Received descriptors are close-on-exec and owned by the result.
	auto ReceiveMessage(int socket_fd) -> ReceivedMessage;

}  // namespace howdy::native::runtime_descriptor_message
