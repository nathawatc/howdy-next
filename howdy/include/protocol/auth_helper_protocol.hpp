#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include <sys/types.h>

namespace howdy::native::auth_helper_protocol {

	inline constexpr const char *kConfigPathKey    = "CONFIG_PATH";
	inline constexpr const char *kUserModelsDirKey = "USER_MODELS_DIR";

	inline constexpr const char      *kPreparedRuntimeRoot                    = "/run/howdy";
	inline constexpr const char      *kPreparedRuntimeDirectoryPrefix         = "pam-";
	inline constexpr std::string_view kPreparedRuntimeDirectorySuffixTemplate = "XXXXXX";
	inline constexpr auto             kPreparedRuntimeDirectorySuffixLength =
	    kPreparedRuntimeDirectorySuffixTemplate.size();
	inline constexpr const char *kPreparedConfigFileName          = "config.ini";
	inline constexpr const char *kPreparedUserModelsDirectoryName = "models";

	inline auto prepared_runtime_root() -> std::filesystem::path {
		return kPreparedRuntimeRoot;
	}

	inline auto prepared_runtime_directory_prefix(uid_t uid) -> std::string {
		return std::string(kPreparedRuntimeDirectoryPrefix) + std::to_string(uid) + "-";
	}

	inline auto prepared_runtime_directory_template(uid_t uid) -> std::string {
		return prepared_runtime_directory_prefix(uid) +
		       std::string(kPreparedRuntimeDirectorySuffixTemplate);
	}

	inline auto prepared_config_path(const std::filesystem::path &runtime_dir)
	    -> std::filesystem::path {
		return runtime_dir / kPreparedConfigFileName;
	}

	inline auto prepared_user_models_dir(const std::filesystem::path &runtime_dir)
	    -> std::filesystem::path {
		return runtime_dir / kPreparedUserModelsDirectoryName;
	}

	inline auto is_canonical_absolute_path(const std::filesystem::path &path) -> bool {
		return path.is_absolute() && path.string() == path.lexically_normal().string();
	}

	inline auto matches_prepared_runtime_layout(const std::filesystem::path &runtime_dir,
	                                            const std::filesystem::path &config_path,
	                                            const std::filesystem::path &user_models_dir,
	                                            uid_t                        uid) -> bool {
		if (!is_canonical_absolute_path(runtime_dir) || !is_canonical_absolute_path(config_path) ||
		    !is_canonical_absolute_path(user_models_dir)) {
			return false;
		}

		if (runtime_dir.parent_path() != prepared_runtime_root()) {
			return false;
		}

		const auto directory_name = runtime_dir.filename().string();
		const auto prefix         = prepared_runtime_directory_prefix(uid);
		if (directory_name.size() != prefix.size() + kPreparedRuntimeDirectorySuffixLength ||
		    !directory_name.starts_with(prefix)) {
			return false;
		}

		return config_path == prepared_config_path(runtime_dir) &&
		       user_models_dir == prepared_user_models_dir(runtime_dir);
	}

}  // namespace howdy::native::auth_helper_protocol
