#include "protocol/runtime_descriptor_message.hpp"
#include "support/bit_flags.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

#include <sys/socket.h>  // IWYU pragma: keep

// Input layout: one byte of recvmsg flags (bit 0 MSG_TRUNC, bit 1 MSG_CTRUNC), one byte whose
// low two bits give the payload length, the payload, then the control bytes.
// NOLINTBEGIN(readability-identifier-naming)
extern "C" auto LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) -> int;

extern "C" auto LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) -> int {
	namespace message = howdy::native::runtime_descriptor_message;
	const auto input  = std::as_bytes(std::span(data, size));
	if (input.size() < 2) {
		return 0;
	}
	const auto selector = std::to_integer<unsigned>(input.front());
	const int  flags    = howdy::native::CombineBitsAs<int>((selector & 1U) != 0 ? MSG_TRUNC : 0,
	                                                        (selector & 2U) != 0 ? MSG_CTRUNC : 0);
	const auto rest     = input.subspan(2);
	const auto payload_size =
	    std::min(std::to_integer<std::size_t>(input.subspan(1, 1).front()) % 4, rest.size());
	const auto payload = rest.first(payload_size);
	const auto control = rest.subspan(payload_size);

	const auto decoded =
	    message::DecodeMessage({.payload = payload, .control = control, .flags = flags});
	if (!decoded.valid) {
		return 0;
	}
	// A message the decoder accepts must match the format exactly.
	const auto state = message::ParseModelState(std::to_integer<char>(payload.back()));
	if (flags != 0 || payload.size() != message::kPayloadSize ||
	    std::to_integer<char>(payload.front()) != message::kMarker || !state.has_value() ||
	    *state != decoded.model_state ||
	    decoded.descriptors.size() != message::ExpectedDescriptorCount(decoded.model_state)) {
		std::abort();
	}
	return 0;
}

// NOLINTEND(readability-identifier-naming)
