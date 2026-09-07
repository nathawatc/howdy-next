#include "config/runtime_config.hpp"
#include "config/runtime_paths.hpp"
#include "support/file_security.hpp"

namespace howdy::native {

	auto LoadRuntimeConfig(const std::filesystem::path &config_path) -> RuntimeConfigLoadResult {
		return LoadRuntimeConfig(config_path, DefaultSecureOwnerUid());
	}

	auto LoadRuntimeConfig() -> RuntimeConfigLoadResult {
		return LoadRuntimeConfig(ResolveConfigPath());
	}

}  // namespace howdy::native
