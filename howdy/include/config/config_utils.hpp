#pragma once

#include "config/config_limits.hpp"
#include "support/atomic_files.hpp"
#include "support/fd_io.hpp"
#include "support/file_security.hpp"
#include "support/file_security/validation_root.hpp"
#include "support/scoped_fd.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>

#include <unistd.h>

#include <sys/stat.h>
#include <sys/types.h>

namespace howdy::native {
	inline constexpr auto kConfigFileLabel                  = "Config file";
	inline constexpr auto kEditedConfigInstallFailedMessage = "Failed to install edited config";

	using ConfigPathCheckResult = SecurePathCheckResult;

	struct SecureConfigOpenResult {
		ScopedFd              fd;
		int                   open_error = 0;
		ConfigPathCheckResult security;
	};

	[[nodiscard]] inline auto ConfigAccessErrorHint(int error_code) -> std::string {
		if (error_code != EACCES || geteuid() == 0) {
			return {};
		}

		return "; process uid=" + std::to_string(getuid()) + " euid=" + std::to_string(geteuid()) +
		       " cannot inspect the restricted Howdy config path. PAM consumers such "
		       "as lock screens must call pam_howdy from a privileged authentication "
		       "helper; do not make /etc/howdy or config.ini world-readable";
	}

	[[nodiscard]] inline auto ApplyConfigAccessErrorHint(ConfigPathCheckResult result)
	    -> ConfigPathCheckResult {
		if (!result.ok) {
			result.error_message += ConfigAccessErrorHint(result.error_code);
		}
		return result;
	}

	[[nodiscard]] inline auto
	CheckSecureConfigFd(int fd, const std::filesystem::path &config_path,
	                    const std::optional<uid_t>                    owner_uid,
	                    const file_security_internal::ValidationRoot &validation_root = {})
	    -> ConfigPathCheckResult {
		return ApplyConfigAccessErrorHint(CheckSecureRootOwnedFdWithDirectory(
		    fd, config_path, {.directory = "Config directory", .file = kConfigFileLabel}, owner_uid,
		    validation_root));
	}

	[[nodiscard]] inline auto
	CheckSecureConfigPath(const std::filesystem::path                  &config_path,
	                      const std::optional<uid_t>                    owner_uid,
	                      const file_security_internal::ValidationRoot &validation_root = {})
	    -> ConfigPathCheckResult {
		return ApplyConfigAccessErrorHint(CheckSecureRootOwnedFileWithDirectory(
		    config_path, {.directory = "Config directory", .file = kConfigFileLabel}, owner_uid,
		    validation_root));
	}

	// How many times a reader opens the config again after finding it replaced; each look follows
	// a write that replaced it, so the bound only stops a stream of writes from starving a reader.
	inline constexpr int kMaxConfigReopens = 4;

	// Whether `fd` names a file that has lost its last link, as one a writer replaced since it was
	// opened. Such a file fails the security check; a second open finds the new one, which has to
	// pass it.
	[[nodiscard]] inline auto ConfigFdWasReplaced(int fd) -> bool {
		struct stat fd_stat{};
		return fstat(fd, &fd_stat) == 0 && fd_stat.st_nlink == 0;
	}

	[[nodiscard]] auto
	OpenSecureConfigForRead(const std::filesystem::path                  &config_path,
	                        std::optional<uid_t>                          owner_uid,
	                        const file_security_internal::ValidationRoot &validation_root = {})
	    -> SecureConfigOpenResult;

	[[nodiscard]] inline auto
	ReadConfigFromFd(int fd, const std::optional<std::size_t> max_bytes = kMaxConfigFileSize)
	    -> std::optional<std::string> {
		// A positional read from the start leaves the descriptor offset alone, which matters when
		// the descriptor was inherited and its open file description is shared.
		std::size_t read_limit = std::numeric_limits<std::size_t>::max();
		if (max_bytes.has_value() && *max_bytes < read_limit) {
			read_limit = *max_bytes + std::size_t{1};
		}
		const auto result =
		    ReadFdToStringBounded({.fd = fd, .max_bytes = read_limit, .offset = off_t{0}});
		if (result.read_error || (max_bytes.has_value() && result.output.size() > *max_bytes)) {
			return std::nullopt;
		}
		return result.output;
	}

	// Messages in the results below are for display only; decide on the status.
	enum class ConfigValidationStatus : std::uint8_t {
		kOk,
		kTooLarge,
		// Blank, or holding a NUL byte.
		kDamaged,
		kUnparsable,
		kInvalidSetting,
	};

	// Fail closed: a result that names no status never passes as a valid config.
	struct ConfigValidationResult {
		ConfigValidationStatus status = ConfigValidationStatus::kUnparsable;
		std::string            message;
	};

	enum class ConfigWriteStatus : std::uint8_t {
		kOk,
		// The option cannot be set, or not to this value.
		kInvalidRequest,
		// The current config is damaged or cannot be edited safely (a duplicate option, an
		// ambiguous continuation line), or the content to install is too large or damaged, does
		// not parse, or holds an invalid setting.
		kInvalidConfig,
		kInsecurePath,
		// The current config could not be opened, inspected or read.
		kReadFailed,
		kLockFailed,
		// The current config is no longer the content the caller started from.
		kStale,
		// The staged file could not be prepared.
		kStagingFailed,
		// Writing or renaming the staged file failed; the old config is in place.
		kInstallFailed,
		// Installed, but its directory was not synced: potentially committed.
		kDurabilityUncertain,
	};

	// Fail closed: a result that names no status is a failure.
	struct ConfigWriteResult {
		ConfigWriteStatus status = ConfigWriteStatus::kInstallFailed;
		std::string       message;
	};

	struct ConfigReplaceOptions {
		bool lock             = true;
		bool validate_runtime = true;
		// When set, the install is refused as kStale unless the current config still matches.
		const std::string                     *expected_current_content = nullptr;
		SyncParentDirectoryFn                  sync_parent              = SyncParentDirectory;
		file_security_internal::ValidationRoot validation_root{};
	};

	struct ConfigUpdateOptions {
		bool                                   lock             = true;
		bool                                   validate_runtime = true;
		file_security_internal::ValidationRoot validation_root{};
	};

	struct ConfigValueUpdate {
		std::string key;
		std::string value;
	};

	[[nodiscard]] auto ValidateConfigContent(const std::string &content) -> ConfigValidationResult;
	[[nodiscard]] auto ReplaceConfigContentAtomically(const std::filesystem::path &config_path,
	                                                  const std::string           &content,
	                                                  const ConfigReplaceOptions  &options = {})
	    -> ConfigWriteResult;
	[[nodiscard]] auto UpdateConfigValue(const std::filesystem::path &config_path,
	                                     const ConfigValueUpdate     &update,
	                                     const ConfigUpdateOptions   &options = {})
	    -> ConfigWriteResult;

}  // namespace howdy::native
