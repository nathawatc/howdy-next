#include "common/file_security.hpp"
#include "config/runtime_config_loader.hpp"
#include "config/runtime_paths.hpp"

namespace howdy::native {

	auto load_runtime_config() -> RuntimeConfigLoadResult {
		return load_runtime_config(resolve_config_path());
	}

	auto load_runtime_config(const std::filesystem::path &path) -> RuntimeConfigLoadResult {
		return load_runtime_config(path, default_secure_owner_uid());
	}

}  // namespace howdy::native
