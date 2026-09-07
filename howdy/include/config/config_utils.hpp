#pragma once

#include "config/config_limits.hpp"
#include "support/atomic_files.hpp"
#include "support/fd_io.hpp"
#include "support/file_security.hpp"

#include <cerrno>
#include <filesystem>
#include <limits>
#include <optional>
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

	inline auto ConfigAccessErrorHint(int error_code) -> std::string {
		if (error_code != EACCES || geteuid() == 0) {
			return {};
		}

		return "; process uid=" + std::to_string(getuid()) + " euid=" + std::to_string(geteuid()) +
		       " cannot inspect the restricted Howdy config path. PAM consumers such "
		       "as lock screens must call pam_howdy from a privileged authentication "
		       "helper; do not make /etc/howdy or config.ini world-readable";
	}

	inline auto CheckSecureConfigFd(int fd, const std::filesystem::path &config_path,
	                                const std::optional<uid_t> owner_uid) -> ConfigPathCheckResult {
		const auto parent = config_path.parent_path();
		if (parent.empty()) {
			return ConfigPathCheckResult{
			    .ok = false,
			    .error_message =
			        "Config file must have a parent directory: " + config_path.string(),
			    .error_code = 0,
			};
		}

		const auto file_security = CheckSecureRootOwnedFdWithDirectory(
		    fd, config_path, {.directory = "Config directory", .file = kConfigFileLabel},
		    owner_uid);
		if (!file_security.ok) {
			return ConfigPathCheckResult{
			    .ok = false,
			    .error_message =
			        file_security.error_message + ConfigAccessErrorHint(file_security.error_code),
			    .error_code = file_security.error_code,
			};
		}

		return ConfigPathCheckResult{.ok = true, .error_message = {}, .error_code = 0};
	}

	inline auto CheckSecureConfigFd(int fd, const std::filesystem::path &config_path)
	    -> ConfigPathCheckResult {
		return CheckSecureConfigFd(fd, config_path, DefaultSecureOwnerUid());
	}

	inline auto CheckSecureConfigPath(const std::filesystem::path &config_path)
	    -> ConfigPathCheckResult;

	inline auto CheckSecureConfigPath(const std::filesystem::path &config_path,
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

		const auto file_security = CheckSecureRootOwnedFileWithDirectory(
		    config_path, {.directory = "Config directory", .file = kConfigFileLabel}, owner_uid);
		if (!file_security.ok) {
			return ConfigPathCheckResult{
			    .ok = false,
			    .error_message =
			        file_security.error_message + ConfigAccessErrorHint(file_security.error_code),
			    .error_code = file_security.error_code,
			};
		}

		return ConfigPathCheckResult{.ok = true, .error_message = {}, .error_code = 0};
	}

	inline auto CheckSecureConfigPath(const std::filesystem::path &config_path)
	    -> ConfigPathCheckResult {
		return CheckSecureConfigPath(config_path, DefaultSecureOwnerUid());
	}

	inline auto ReadConfigFromFd(int                              fd,
	                             const std::optional<std::size_t> max_bytes = kMaxConfigFileSize)
	    -> std::optional<std::string> {
		if (lseek(fd, 0, SEEK_SET) < 0) {
			return std::nullopt;
		}

		const auto read_limit = max_bytes.has_value() ? *max_bytes + std::size_t{1}
		                                              : std::numeric_limits<std::size_t>::max();
		const auto result     = ReadFdToStringBounded({.fd = fd, .max_bytes = read_limit});
		if (result.read_error || (max_bytes.has_value() && result.output.size() > *max_bytes)) {
			return std::nullopt;
		}
		return result.output;
	}

	auto IsSafeIniScalarValue(std::string_view value) -> bool;
	auto ReadConfigLines(const std::filesystem::path &config_path, bool lock = false)
	    -> std::vector<std::string>;
	auto AtomicWriteLines(const std::filesystem::path    &config_path,
	                      const std::vector<std::string> &lines,
	                      SyncParentDirectoryFn           sync_parent = SyncParentDirectory)
	    -> AtomicFileCommitResult;
	auto ValidateConfigContent(const std::string &content, std::string *error_message) -> bool;
	auto ReplaceConfigContentAtomically(const std::filesystem::path &config_path,
	                                    const std::string           &content,
	                                    std::string *error_message = nullptr, bool lock = true,
	                                    bool                  validate_runtime         = true,
	                                    const std::string    *expected_current_content = nullptr,
	                                    SyncParentDirectoryFn sync_parent = SyncParentDirectory)
	    -> bool;
	auto UpdateConfigValue(const std::filesystem::path &config_path, const std::string &key,
	                       std::string *error_message, const std::string &value, bool lock = false,
	                       bool validate_runtime = true) -> bool;

	inline auto UpdateConfigValue(const std::filesystem::path &config_path, const std::string &key,
	                              const std::string &value, bool lock = false) -> bool {
		return UpdateConfigValue(config_path, key, nullptr, value, lock, true);
	}

}  // namespace howdy::native
