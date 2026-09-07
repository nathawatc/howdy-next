#include "config/runtime_paths.hpp"

#include "paths.hpp"

#include <cstdlib>
#include <filesystem>
#include <unistd.h>

namespace howdy::native {

	namespace {

		auto AllowEnvPathOverrides() -> bool {
			return geteuid() == getuid() && getegid() == getgid();
		}

		auto PathFromEnv(const char *name) -> std::filesystem::path {
			if (!AllowEnvPathOverrides()) {
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

		auto SelectRuntimePath(const char *env_name, const std::filesystem::path &configured_path)
		    -> std::filesystem::path {
			const auto env_path = PathFromEnv(env_name);
			return env_path.empty() ? configured_path : env_path;
		}

	}  // namespace

	auto ResolveConfigPath() -> std::filesystem::path {
		return SelectRuntimePath("HOWDY_CONFIG", kConfiguredConfigPath);
	}

	auto ResolveModelsDir() -> std::filesystem::path {
		return SelectRuntimePath("HOWDY_MODELS_DIR", kConfiguredModelsDir);
	}

	auto ResolveUserModelsDir() -> std::filesystem::path {
		return SelectRuntimePath("HOWDY_USER_MODELS_DIR", kConfiguredUserModelsDir);
	}

	auto ResolveLogPath() -> std::filesystem::path {
		return SelectRuntimePath("HOWDY_LOG_PATH", kConfiguredLogPath);
	}

}  // namespace howdy::native
