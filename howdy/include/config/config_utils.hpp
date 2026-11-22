#pragma once

#include "config/config_limits.hpp"
#include "support/atomic_files.hpp"
#include "support/fd_io.hpp"
#include "support/file_security.hpp"
#include "support/file_security/validation_root.hpp"
#include "support/scoped_fd.hpp"

#include <cerrno>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <utility>

#include <unistd.h>

#include <sys/types.h>

namespace howdy::native {
	inline constexpr auto kConfigFileLabel                  = "Config file";
	inline constexpr auto kUpdatedConfigInvalidMessage      = "Updated config is invalid";
	inline constexpr auto kEditedConfigInstallFailedMessage = "Failed to install edited config";
	inline constexpr auto kStaleEditedConfigMessage =
	    "Config changed while editing; not installing stale edited config";

	using ConfigPathCheckResult = SecurePathCheckResult;

	struct SecureConfigOpenResult {
		ScopedFd              fd;
		int                   open_error = 0;
		ConfigPathCheckResult security;
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

	inline auto ApplyConfigAccessErrorHint(ConfigPathCheckResult result) -> ConfigPathCheckResult {
		if (!result.ok) {
			result.error_message += ConfigAccessErrorHint(result.error_code);
		}
		return result;
	}

	inline auto
	CheckSecureConfigFd(int fd, const std::filesystem::path &config_path,
	                    const std::optional<uid_t>                    owner_uid,
	                    const file_security_internal::ValidationRoot &validation_root = {})
	    -> ConfigPathCheckResult {
		return ApplyConfigAccessErrorHint(CheckSecureRootOwnedFdWithDirectory(
		    fd, config_path, {.directory = "Config directory", .file = kConfigFileLabel}, owner_uid,
		    validation_root));
	}

	inline auto
	CheckSecureConfigPath(const std::filesystem::path                  &config_path,
	                      const std::optional<uid_t>                    owner_uid,
	                      const file_security_internal::ValidationRoot &validation_root = {})
	    -> ConfigPathCheckResult {
		return ApplyConfigAccessErrorHint(CheckSecureRootOwnedFileWithDirectory(
		    config_path, {.directory = "Config directory", .file = kConfigFileLabel}, owner_uid,
		    validation_root));
	}

	auto OpenSecureConfigForRead(const std::filesystem::path                  &config_path,
	                             std::optional<uid_t>                          owner_uid,
	                             const file_security_internal::ValidationRoot &validation_root = {})
	    -> SecureConfigOpenResult;

	inline auto ReadConfigFromFd(int                              fd,
	                             const std::optional<std::size_t> max_bytes = kMaxConfigFileSize)
	    -> std::optional<std::string> {
		// A positional read from the start leaves the descriptor offset alone, which matters when
		// the descriptor was inherited and its open file description is shared.
		const auto read_limit = max_bytes.has_value() ? *max_bytes + std::size_t{1}
		                                              : std::numeric_limits<std::size_t>::max();
		const auto result =
		    ReadFdToStringBounded({.fd = fd, .max_bytes = read_limit, .offset = off_t{0}});
		if (result.read_error || (max_bytes.has_value() && result.output.size() > *max_bytes)) {
			return std::nullopt;
		}
		return result.output;
	}

	auto ValidateConfigContent(const std::string &content, std::string *error_message) -> bool;
	auto ReplaceConfigContentAtomically(
	    const std::filesystem::path &config_path, const std::string &content,
	    std::string *error_message = nullptr, bool lock = true, bool validate_runtime = true,
	    const std::string                            *expected_current_content = nullptr,
	    SyncParentDirectoryFn                         sync_parent     = SyncParentDirectory,
	    const file_security_internal::ValidationRoot &validation_root = {}) -> bool;
	auto UpdateConfigValue(const std::filesystem::path &config_path, const std::string &key,
	                       std::string *error_message, const std::string &value, bool lock = false,
	                       bool                                          validate_runtime = true,
	                       const file_security_internal::ValidationRoot &validation_root  = {})
	    -> bool;

}  // namespace howdy::native
