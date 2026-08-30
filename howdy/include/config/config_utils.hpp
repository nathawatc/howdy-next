#pragma once

#include "support/atomic_files.hpp"
#include "support/file_security.hpp"

#include <cerrno>
#include <filesystem>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace howdy::native {
	inline constexpr auto kConfigFileLabel                  = "Config file";
	inline constexpr auto kUpdatedConfigInvalidMessage      = "Updated config is invalid";
	inline constexpr auto kEditedConfigInstallFailedMessage = "Failed to install edited config";
	inline constexpr auto kStaleEditedConfigMessage =
	    "Config changed while editing; not installing stale edited config";

	struct ConfigPathCheckResult {
		bool        ok = false;
		std::string error_message;
		int         error_code = 0;
	};

	inline auto config_access_error_hint(int error_code) -> std::string {
		if (error_code != EACCES || geteuid() == 0) {
			return {};
		}

		return "; process uid=" + std::to_string(getuid()) + " euid=" + std::to_string(geteuid()) +
		       " cannot inspect the restricted Howdy config path. PAM consumers such "
		       "as lock screens must call pam_howdy from a privileged authentication "
		       "helper; do not make /etc/howdy or config.ini world-readable";
	}

	inline auto check_secure_config_path(const std::filesystem::path &config_path)
	    -> ConfigPathCheckResult;

	inline auto check_secure_config_path(const std::filesystem::path &config_path,
	                                     const std::optional<uid_t>   owner_uid)
	    -> ConfigPathCheckResult {
		const auto parent = config_path.parent_path();
		if (parent.empty()) {
			return ConfigPathCheckResult{
			    .ok = false,
			    .error_message =
			        "Config file must have a parent directory: " + config_path.string(),
			    .error_code = 0,
			};
		}

		const auto file_security = check_secure_root_owned_file_with_directory(
		    config_path, {.directory = "Config directory", .file = kConfigFileLabel}, owner_uid);
		if (!file_security.ok) {
			return ConfigPathCheckResult{
			    .ok            = false,
			    .error_message = file_security.error_message +
			                     config_access_error_hint(file_security.error_code),
			    .error_code    = file_security.error_code,
			};
		}

		return ConfigPathCheckResult{.ok = true, .error_message = {}, .error_code = 0};
	}

	inline auto check_secure_config_path(const std::filesystem::path &config_path)
	    -> ConfigPathCheckResult {
		return check_secure_config_path(config_path, default_secure_owner_uid());
	}

	auto is_safe_ini_scalar_value(std::string_view value) -> bool;
	auto read_config_lines(const std::filesystem::path &config_path, bool lock = false)
	    -> std::vector<std::string>;
	auto atomic_write_lines(const std::filesystem::path    &config_path,
	                        const std::vector<std::string> &lines,
	                        SyncParentDirectoryFn           sync_parent = sync_parent_directory)
	    -> AtomicFileCommitResult;
	auto validate_config_content(const std::string &content, std::string *error_message) -> bool;
	auto replace_config_content_atomically(
	    const std::filesystem::path &config_path, const std::string &content,
	    std::string *error_message = nullptr, bool lock = true, bool validate_runtime = true,
	    const std::string    *expected_current_content = nullptr,
	    SyncParentDirectoryFn sync_parent              = sync_parent_directory) -> bool;
	auto update_config_value(const std::filesystem::path &config_path, const std::string &key,
	                         std::string *error_message, const std::string &value,
	                         bool lock = false, bool validate_runtime = true) -> bool;

	inline auto update_config_value(const std::filesystem::path &config_path,
	                                const std::string &key, const std::string &value,
	                                bool lock = false) -> bool {
		return update_config_value(config_path, key, nullptr, value, lock, true);
	}

}  // namespace howdy::native
