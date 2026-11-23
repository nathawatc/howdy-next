#pragma once

#include "support/atomic_files.hpp"
#include "support/file_lock.hpp"
#include "support/file_security/validation_root.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace howdy::native::config_utils_internal {

	constexpr auto kConfigFileOpenFailureMessage    = "Failed to open config file";
	constexpr auto kConfigFileInspectFailureMessage = "Failed to inspect config file";
	constexpr auto kConfigFileReadFailureMessage    = "Failed to read config file";
	constexpr auto kConfigFileLockFailureMessage    = "Failed to lock config file";
	constexpr auto kUpdatedConfigTooLargeMessage    = "Updated config exceeds maximum size";

	inline auto FailWith(std::string *error_message, std::string_view message) -> bool {
		if (error_message != nullptr) {
			*error_message = message;
		}
		return false;
	}

	[[nodiscard]] __attribute__((visibility("hidden"))) auto
	AcquireConfigLock(ScopedFileLock &guard, const std::filesystem::path &config_path,
	                  std::string *error_message) -> bool;

	[[nodiscard]] __attribute__((visibility("hidden"))) auto
	ExpectedContentMatches(const std::filesystem::path &config_path, const std::string &expected,
	                       std::string                                  *error_message,
	                       const file_security_internal::ValidationRoot &validation_root) -> bool;

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

	[[nodiscard]] __attribute__((visibility("hidden"))) auto
	IsSafeIniScalarValue(std::string_view value) -> bool;

}  // namespace howdy::native::config_utils_internal
