#include "support/rust_ffi.hpp"

#include "howdy_ffi.h"

#include <cstdint>

namespace howdy::native {

	auto RustFfiAbiMatches() -> bool {
		std::uint32_t version = 0;
		return howdy_ffi_abi_version(&version) == HOWDY_FFI_STATUS_OK &&
		       version == HOWDY_FFI_ABI_VERSION;
	}

}  // namespace howdy::native
