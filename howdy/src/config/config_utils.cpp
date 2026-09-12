#include "config/config_utils.hpp"

#include "config/config_limits.hpp"
#include "config/config_schema.hpp"
#include "config/test_hooks.hpp"
#include "config_utils/internal.hpp"

#include <algorithm>
#include <fcntl.h>
#include <filesystem>
#include <string>

#include <sys/stat.h>

namespace howdy::native {

	namespace {

		auto FailWith(std::string *error_message, const std::string &message) -> bool {
			if (error_message != nullptr) {
				*error_message = message;
			}
			return false;
		}

	}  // namespace

	auto ReplaceConfigContentAtomically(
	    const std::filesystem::path &config_path, const std::string &content,
	    std::string *error_message, bool lock, bool validate_runtime,
	    const std::string *expected_current_content, SyncParentDirectoryFn sync_parent,
	    const file_security_internal::ValidationRoot &validation_root) -> bool {
		using namespace config_utils_internal;

		if (error_message != nullptr) {
			error_message->clear();
		}
		if (content.size() > kMaxConfigFileSize) {
			return FailWith(error_message, kUpdatedConfigTooLargeMessage);
		}

		const auto initial_security =
		    CheckSecureConfigPath(config_path, DefaultSecureOwnerUid(), validation_root);
		if (!initial_security.ok) {
			return FailWith(error_message, initial_security.error_message);
		}

		ConfigLockGuard config_lock;
		if (lock && !AcquireConfigLock(config_lock, config_path)) {
			return FailWith(error_message, kConfigFileLockFailureMessage);
		}

		if (validate_runtime && !ValidateConfigContent(content, error_message)) {
			return false;
		}

		const auto final_security =
		    CheckSecureConfigPath(config_path, DefaultSecureOwnerUid(), validation_root);
		if (!final_security.ok) {
			return FailWith(error_message, final_security.error_message);
		}

		struct stat current_stat{};
		if (lstat(config_path.c_str(), &current_stat) != 0 || !S_ISREG(current_stat.st_mode)) {
			return FailWith(error_message, kConfigFileInspectFailureMessage);
		}

		if (expected_current_content != nullptr) {
			if (!ExpectedContentMatches(config_path, *expected_current_content, error_message,
			                            validation_root)) {
				return false;
			}
		}
		const auto install_result =
		    InstallConfigContent(config_path, content, current_stat, sync_parent);
		if (install_result == ConfigInstallResult::kCommittedNotDurable) {
			return FailWith(error_message,
			                "Config was installed, but its directory could not be synced; verify "
			                "state before retrying");
		}
		if (install_result == ConfigInstallResult::kStageFailed) {
			return FailWith(error_message, "Failed to stage updated config");
		}
		if (install_result == ConfigInstallResult::kNotCommitted) {
			return FailWith(error_message, kEditedConfigInstallFailedMessage);
		}
		return true;
	}

	auto UpdateConfigValue(const std::filesystem::path &config_path, const std::string &key,
	                       std::string *error_message, const std::string &value, bool lock,
	                       bool                                          validate_runtime,
	                       const file_security_internal::ValidationRoot &validation_root) -> bool {
		using namespace config_utils_internal;

		if (!IsSafeIniScalarValue(value)) {
			return FailWith(error_message,
			                "Config values must be single-line scalars and cannot start with [");
		}

		const auto initial_security =
		    CheckSecureConfigPath(config_path, DefaultSecureOwnerUid(), validation_root);
		if (!initial_security.ok) {
			return FailWith(error_message, initial_security.error_message);
		}

		ConfigLockGuard config_lock;
		if (lock && !AcquireConfigLock(config_lock, config_path)) {
			return FailWith(error_message, kConfigFileLockFailureMessage);
		}

		ScopedFd fd(open(config_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
		if (fd.Get() < 0) {
			return FailWith(error_message, kConfigFileOpenFailureMessage);
		}
		if (config_test_hooks::Current()) {
			config_test_hooks::Current()();
		}

		const auto security =
		    CheckSecureConfigFd(fd.Get(), config_path, DefaultSecureOwnerUid(), validation_root);
		if (!security.ok) {
			return FailWith(error_message, security.error_message);
		}

		const auto current_content = ReadConfigFromFd(fd.Get());
		fd.Reset();
		if (!current_content.has_value()) {
			return FailWith(error_message, kConfigFileReadFailureMessage);
		}
		auto lines = SplitLinesPreserveNewlines(*current_content);

		const auto options = config_schema::RuntimeConfigOptions();
		const auto option  = std::ranges::find_if(options, [&](const auto &candidate) -> auto {
			return candidate.key == key;
		});
		if (option == options.end()) {
			return FailWith(error_message, "Could not find a \"" + key + "\" config option to set");
		}
		const auto replace_result =
		    ReplaceLineValue(lines, {.section = option->section, .key = key, .value = value});
		if (replace_result == ConfigLineReplaceResult::kDuplicate) {
			return FailWith(error_message, "Config option \"" + key +
			                                   "\" appears more than once in section [" +
			                                   std::string(option->section) + "]");
		}
		if (replace_result == ConfigLineReplaceResult::kNotFound) {
			return FailWith(error_message, "Could not find a \"" + key + "\" config option to set");
		}

		const auto updated_content = JoinLines(lines);
		if (validate_runtime && !ValidateConfigContent(updated_content, error_message)) {
			return false;
		}

		std::string install_error;
		const bool  ok = ReplaceConfigContentAtomically(
		    config_path, updated_content, error_message == nullptr ? nullptr : &install_error,
		    false, false, &*current_content, SyncParentDirectory, validation_root);
		if (!ok && error_message != nullptr) {
			*error_message =
			    install_error.empty() || install_error == kEditedConfigInstallFailedMessage
			        ? "Failed to update config file"
			        : install_error;
		}
		return ok;
	}

}  // namespace howdy::native
