#pragma once

#include "config/config_utils.hpp"
#include "support/atomic_files.hpp"
#include "support/file_lock.hpp"
#include "support/file_security/validation_root.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace howdy::native::config_utils_internal {

	constexpr auto kConfigFileOpenFailureMessage    = "Failed to open config file";
	constexpr auto kConfigFileInspectFailureMessage = "Failed to inspect config file";
	constexpr auto kConfigFileReadFailureMessage    = "Failed to read config file";
	constexpr auto kConfigFileLockFailureMessage    = "Failed to lock config file";
	constexpr auto kUpdatedConfigInvalidMessage     = "Updated config is invalid";
	constexpr auto kUpdatedConfigTooLargeMessage    = "Updated config exceeds maximum size";
	constexpr auto kStaleEditedConfigMessage =
	    "Config changed while editing; not installing stale edited config";

	// The held lock, or a message saying why it could not be taken.
	[[nodiscard]] __attribute__((visibility("hidden"))) auto
	AcquireConfigLock(const std::filesystem::path &config_path)
	    -> std::expected<ScopedFileLock, std::string>;

	// The result of a failed check. An errno says that the path could not be inspected, not that
	// it is insecure.
	[[nodiscard]] inline auto PathFailure(ConfigPathCheckResult check) -> ConfigWriteResult {
		return {
		    .status  = check.error_code == 0 ? ConfigWriteStatus::kInsecurePath
			                                 : ConfigWriteStatus::kReadFailed,
		    .message = std::move(check.error_message),
		};
	}

	// The result of a failed open. ELOOP is a symlink put there since the path was checked.
	[[nodiscard]] inline auto OpenFailure(int open_error) -> ConfigWriteResult {
		return {
		    .status  = open_error == ELOOP ? ConfigWriteStatus::kInsecurePath
			                               : ConfigWriteStatus::kReadFailed,
		    .message = kConfigFileOpenFailureMessage,
		};
	}

	// kOk, or kStale, kInsecurePath or kReadFailed.
	[[nodiscard]] __attribute__((visibility("hidden"))) auto
	CheckExpectedContent(const std::filesystem::path &config_path, const std::string &expected,
	                     const file_security_internal::ValidationRoot &validation_root)
	    -> ConfigWriteResult;

	[[nodiscard]] __attribute__((visibility("hidden"))) auto
	InstallConfigContent(const std::filesystem::path &config_path, const std::string &content,
	                     SyncParentDirectoryFn sync_parent)
	    -> std::optional<AtomicFileCommitResult>;

	struct ConfigLineReplacement {
		std::string_view section;
		std::string_view key;
		std::string_view value;
	};

	enum class ConfigLineReplaceResult : std::uint8_t {
		kNotFound,
		kReplaced,
		kDuplicate,
		// A line continuing the old value looks like a setting or section of its own.
		kAmbiguousContinuation,
	};

	struct ConfigLineReplaceOutcome {
		ConfigLineReplaceResult result = ConfigLineReplaceResult::kNotFound;
		std::size_t             line   = 0;  // 1-based, set for kAmbiguousContinuation
	};

	[[nodiscard]] __attribute__((visibility("hidden"))) auto
	SplitLinesPreserveNewlines(const std::string &content) -> std::vector<std::string>;

	[[nodiscard]] __attribute__((visibility("hidden"))) auto
	JoinLines(const std::vector<std::string> &lines) -> std::string;

	[[nodiscard]] __attribute__((visibility("hidden"))) auto
	ReplaceLineValue(std::vector<std::string> &lines, ConfigLineReplacement replacement)
	    -> ConfigLineReplaceOutcome;

	// Whether the value stays on its line and cannot be taken for a section header.
	[[nodiscard]] __attribute__((visibility("hidden"))) auto
	IsSafeIniScalarValue(std::string_view value) -> bool;

	// Whether inih reads a value ReplaceLineValue wrote as is, without trimming it or cutting it at
	// an inline comment, as long as the line fits its limit.
	[[nodiscard]] __attribute__((visibility("hidden"))) auto
	ReadsBackUnchanged(std::string_view value) -> bool;

}  // namespace howdy::native::config_utils_internal
