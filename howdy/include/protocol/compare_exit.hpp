#pragma once

#include <cstdint>

namespace howdy::native {

	enum class CompareExit : std::uint8_t {
		kSuccess        = 0,
		kAbort          = 12,
		kNoFaceModel    = 10,
		kTimeoutReached = 11,
		kTooDark        = 13,
		kInvalidDevice  = 14,
	};

}  // namespace howdy::native
