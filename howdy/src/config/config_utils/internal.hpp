#pragma once

#include "support/atomic_files.hpp"
#include "support/file_lock.hpp"
#include "support/file_security/validation_root.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace howdy::native::config_utils_internal {

	constexpr auto kConfigFileOpenFailureMessage    = "Failed to open config file";
	constexpr auto kConfigFileInspectFailureMessage = "Failed to inspect config file";
	constexpr auto kConfigFileReadFailureMessage    = "Failed to read config file";
	constexpr auto kConfigFileLockFailureMessage    = "Failed to lock config file";
	constexpr auto kUpdatedConfigTooLargeMessage    = "Updated config exceeds maximum size";

	[[nodiscard]] __attribute__((visibility("hidden"))) auto
	AcquireConfigLock(ScopedFileLock &guard, const std::filesystem::path &config_path) -> bool;

	[[nodiscard]] __attribute__((visibility("hidden"))) auto
	ExpectedContentMatches(const std::filesystem::path &config_path, const std::string &expected,
	                       std::string                                  *error_message,
	                       const file_security_internal::ValidationRoot &validation_root) -> bool;

	[[nodiscard]] __attribute__((visibility("hidden"))) auto
	InstallConfigContent(const std::filesystem::path &config_path, const std::string &content,
	                     SyncParentDirectoryFn sync_parent)
	    -> std::optional<AtomicFileCommitResult>;

}  // namespace howdy::native::config_utils_internal
