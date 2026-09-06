#include "config/runtime_config.hpp"
#include "config/runtime_paths.hpp"
#include "support/file_security.hpp"

namespace howdy::native {

	auto load_runtime_config(const std::filesystem::path &config_path) -> RuntimeConfigLoadResult {
		return load_runtime_config(config_path, default_secure_owner_uid());
	}

	auto load_runtime_config() -> RuntimeConfigLoadResult {
		return load_runtime_config(resolve_config_path());
	}

}  // namespace howdy::native
