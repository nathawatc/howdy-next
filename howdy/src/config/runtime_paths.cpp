#ifndef _GNU_SOURCE
#	define _GNU_SOURCE
#endif

#include "config/runtime_paths.hpp"

#include "paths.hpp"

#include <filesystem>

#include <stdlib.h>
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

			if (const char *value = secure_getenv(name); value != nullptr && *value != '\0') {
				std::filesystem::path path(value);
				// Relative overrides keep their long-standing silent fallback to the configured
				// path. An absolute '..' override is returned unchanged so the file_security.hpp
				// helpers reject it; normalizing it here would quietly pick another directory.
				if (path.is_absolute()) {
					return path;
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
		return SelectRuntimePath(kConfigPathEnvVar, kConfiguredConfigPath);
	}

	auto ResolveModelsDir() -> std::filesystem::path {
		return SelectRuntimePath(kModelsDirEnvVar, kConfiguredModelsDir);
	}

	auto ResolveUserModelsDir() -> std::filesystem::path {
		return SelectRuntimePath(kUserModelsDirEnvVar, kConfiguredUserModelsDir);
	}

	auto ResolveLogPath() -> std::filesystem::path {
		return SelectRuntimePath(kLogPathEnvVar, kConfiguredLogPath);
	}

}  // namespace howdy::native
