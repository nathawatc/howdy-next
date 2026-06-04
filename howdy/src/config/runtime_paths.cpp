#include "config/runtime_paths.hpp"

#include "paths.hpp"

#include <cstdlib>
#include <filesystem>
#include <unistd.h>
#include <vector>

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
                    return value;
                }
            }
            return {};
        }

        auto select_existing_path(const std::filesystem::path              &env_path,
                                  const std::vector<std::filesystem::path> &candidates,
                                  const std::filesystem::path &fallback) -> std::filesystem::path {
            if (!env_path.empty()) {
                return env_path;
            }

            for (const auto &candidate : candidates) {
                std::error_code ec;
                if (!candidate.empty() && std::filesystem::exists(candidate, ec) && !ec) {
                    return candidate;
                }
            }

            return fallback;
        }

    }  // namespace

    auto resolve_config_path() -> std::filesystem::path {
        return select_existing_path(path_from_env("HOWDY_CONFIG"),
                                    {
                                        "/etc/howdy/config.ini",
                                        kConfiguredConfigPath,
                                        kDefaultDevConfigPath,
                                    },
                                    kConfiguredConfigPath);
    }

    auto resolve_models_dir() -> std::filesystem::path {
        return select_existing_path(path_from_env("HOWDY_MODELS_DIR"),
                                    {
                                        "/usr/share/howdy/models",
                                        kConfiguredModelsDir,
                                    },
                                    kConfiguredModelsDir);
    }

    auto resolve_user_models_dir() -> std::filesystem::path {
        return select_existing_path(path_from_env("HOWDY_USER_MODELS_DIR"),
                                    {
                                        "/etc/howdy/models",
                                        kConfiguredUserModelsDir,
                                    },
                                    kConfiguredUserModelsDir);
    }

    auto resolve_log_path() -> std::filesystem::path {
        return select_existing_path(path_from_env("HOWDY_LOG_PATH"),
                                    {
                                        "/var/log/howdy",
                                        kConfiguredLogPath,
                                    },
                                    kConfiguredLogPath);
    }

}  // namespace howdy::native
