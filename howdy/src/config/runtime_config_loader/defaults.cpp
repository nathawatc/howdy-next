#include "config/runtime_config_loader.hpp"
#include "config/runtime_paths.hpp"
#include "support/file_security.hpp"

namespace howdy::native {

	auto LoadRuntimeConfig() -> RuntimeConfigLoadResult {
		return LoadRuntimeConfig(ResolveConfigPath(), DefaultSecureOwnerUid());
	}

}  // namespace howdy::native
