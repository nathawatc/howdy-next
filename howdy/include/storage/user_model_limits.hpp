#pragma once

#include <cstddef>
#include <cstdint>

namespace howdy::native::user_model_limits {

	inline constexpr std::uintmax_t kMaxUserModelFileBytes = std::uintmax_t{1024} * 1024;
	inline constexpr std::size_t    kMaxStoredModels       = 256;
	inline constexpr std::size_t    kMaxEncodingsPerModel  = 32;
	inline constexpr std::size_t    kMaxEncodingLength     = 1024;
	inline constexpr std::size_t    kMaxJsonNestingDepth   = 64;

}  // namespace howdy::native::user_model_limits
