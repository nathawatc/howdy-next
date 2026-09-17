#pragma once

#include "support/atomic_files.hpp"
#include "support/file_security/validation_root.hpp"
#include "support/scoped_fd.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <sys/stat.h>

namespace howdy::native::config_utils_internal {

	constexpr auto kConfigFileOpenFailureMessage    = "Failed to open config file";
	constexpr auto kConfigFileInspectFailureMessage = "Failed to inspect config file";
	constexpr auto kConfigFileReadFailureMessage    = "Failed to read config file";
	constexpr auto kConfigFileLockFailureMessage    = "Failed to lock config file";
	constexpr auto kUpdatedConfigTooLargeMessage    = "Updated config exceeds maximum size";

	struct __attribute__((visibility("hidden"))) ConfigLockGuard {
		ScopedFd fd;

		ConfigLockGuard() = default;
		~ConfigLockGuard();
		ConfigLockGuard(const ConfigLockGuard &)                     = delete;
		auto operator=(const ConfigLockGuard &) -> ConfigLockGuard & = delete;
		ConfigLockGuard(ConfigLockGuard &&other) noexcept;
		auto operator=(ConfigLockGuard &&other) noexcept -> ConfigLockGuard &;
	};

	[[nodiscard]] __attribute__((visibility("hidden"))) auto
	AcquireConfigLock(ConfigLockGuard &guard, const std::filesystem::path &config_path) -> bool;

	[[nodiscard]] __attribute__((visibility("hidden"))) auto
	ExpectedContentMatches(const std::filesystem::path &config_path, const std::string &expected,
	                       std::string                                  *error_message,
	                       const file_security_internal::ValidationRoot &validation_root) -> bool;

	enum class ConfigInstallResult : std::uint8_t {
		kOk,
		kStageFailed,
		kNotCommitted,
		kCommittedNotDurable,
	};

	[[nodiscard]] __attribute__((visibility("hidden"))) auto
	InstallConfigContent(const std::filesystem::path &config_path, const std::string &content,
	                     const struct stat &current_stat, SyncParentDirectoryFn sync_parent)
	    -> ConfigInstallResult;

	struct ConfigLineReplacement {
		std::string_view   section;
		const std::string &key;
		const std::string &value;
	};

	enum class ConfigLineReplaceResult : std::uint8_t {
		kNotFound,
		kReplaced,
		kDuplicate,
	};

	[[nodiscard]] __attribute__((visibility("hidden"))) auto
	SplitLinesPreserveNewlines(const std::string &content) -> std::vector<std::string>;

	[[nodiscard]] __attribute__((visibility("hidden"))) auto
	JoinLines(const std::vector<std::string> &lines) -> std::string;

	[[nodiscard]] __attribute__((visibility("hidden"))) auto
	ReplaceLineValue(std::vector<std::string> &lines, ConfigLineReplacement replacement)
	    -> ConfigLineReplaceResult;

}  // namespace howdy::native::config_utils_internal
