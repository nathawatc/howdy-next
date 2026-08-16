#include "config/runtime_paths.hpp"

#include "paths.hpp"

#include <cstdlib>
#include <filesystem>
#include <unistd.h>

namespace howdy::native {

	namespace {

		auto allow_env_path_overrides() -> bool {
			return geteuid() == getuid() && getegid() == getgid();
		}

		auto path_from_env(const char *name) -> std::filesystem::path {
			if (!allow_env_path_overrides()) {
				return {};
			}

			if (const char *value = std::getenv(name)) {
				if (value[0] != '\0') {
					std::filesystem::path path(value);
					if (path.is_absolute()) {
						return path;
					}
				}
			}
			return {};
		}

		auto select_runtime_path(const char *env_name, const std::filesystem::path &configured_path)
		    -> std::filesystem::path {
			const auto env_path = path_from_env(env_name);
			return env_path.empty() ? configured_path : env_path;
		}

	}  // namespace

	auto resolve_config_path() -> std::filesystem::path {
		return select_runtime_path("HOWDY_CONFIG", kConfiguredConfigPath);
	}

	auto resolve_models_dir() -> std::filesystem::path {
		return select_runtime_path("HOWDY_MODELS_DIR", kConfiguredModelsDir);
	}

	auto resolve_user_models_dir() -> std::filesystem::path {
		return select_runtime_path("HOWDY_USER_MODELS_DIR", kConfiguredUserModelsDir);
	}

	auto resolve_log_path() -> std::filesystem::path {
		return select_runtime_path("HOWDY_LOG_PATH", kConfiguredLogPath);
	}

}  // namespace howdy::native
