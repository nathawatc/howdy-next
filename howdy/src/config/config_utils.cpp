#include "config/config_utils.hpp"

#include "config/config_limits.hpp"
#include "config/config_schema.hpp"
#include "config/test_hooks.hpp"
#include "config_utils/internal.hpp"
#include "support/atomic_files.hpp"
#include "support/file_lock.hpp"
#include "support/file_security.hpp"
#include "support/file_security/validation_root.hpp"
#include "support/scoped_fd.hpp"

#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

#include <fcntl.h>

#include <sys/stat.h>
#include <sys/types.h>

namespace howdy::native {
	namespace config_test_hooks {

		auto Current() -> AfterOpenBeforeRead & {
			static AfterOpenBeforeRead hook;
			return hook;
		}

		ScopedHooks::ScopedHooks(AfterOpenBeforeRead hook)
		    : previous_(std::move(Current())) {
			Current() = std::move(hook);
		}

		ScopedHooks::~ScopedHooks() {
			Current() = std::move(previous_);
		}

	}  // namespace config_test_hooks

	namespace {

		auto Failure(ConfigWriteStatus status, std::string message) -> ConfigWriteResult {
			return {.status = status, .message = std::move(message)};
		}

	}  // namespace

	auto OpenSecureConfigForRead(const std::filesystem::path                  &config_path,
	                             const std::optional<uid_t>                    owner_uid,
	                             const file_security_internal::ValidationRoot &validation_root)
	    -> SecureConfigOpenResult {
		ScopedFd fd(open(config_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
		if (!fd.Valid()) {
			return {.fd = {}, .open_error = errno, .security = {}};
		}
		if (config_test_hooks::Current()) {
			config_test_hooks::Current()();
		}
		auto security = CheckSecureConfigFd(fd.Get(), config_path, owner_uid, validation_root);
		return {.fd = std::move(fd), .open_error = 0, .security = std::move(security)};
	}

	auto ReplaceConfigContentAtomically(const std::filesystem::path &config_path,
	                                    const std::string           &content,
	                                    const ConfigReplaceOptions  &options) -> ConfigWriteResult {
		if (content.size() > kMaxConfigFileSize) {
			return Failure(ConfigWriteStatus::kInvalidConfig,
			               config_utils_internal::kUpdatedConfigTooLargeMessage);
		}

		auto initial_security =
		    CheckSecureConfigPath(config_path, DefaultSecureOwnerUid(), options.validation_root);
		if (!initial_security.ok) {
			return Failure(ConfigWriteStatus::kInsecurePath,
			               std::move(initial_security.error_message));
		}

		ScopedFileLock config_lock;
		if (options.lock) {
			auto acquired = config_utils_internal::AcquireConfigLock(config_path);
			if (!acquired.has_value()) {
				return Failure(ConfigWriteStatus::kLockFailed, std::move(acquired.error()));
			}
			config_lock = std::move(*acquired);
		}

		if (options.validate_runtime) {
			if (auto validation = ValidateConfigContent(content);
			    validation.status != ConfigValidationStatus::kOk) {
				return Failure(ConfigWriteStatus::kInvalidConfig, std::move(validation.message));
			}
		}

		auto final_security =
		    CheckSecureConfigPath(config_path, DefaultSecureOwnerUid(), options.validation_root);
		if (!final_security.ok) {
			return Failure(ConfigWriteStatus::kInsecurePath,
			               std::move(final_security.error_message));
		}

		struct stat current_stat{};
		if (lstat(config_path.c_str(), &current_stat) != 0 || !S_ISREG(current_stat.st_mode)) {
			return Failure(ConfigWriteStatus::kReadFailed,
			               config_utils_internal::kConfigFileInspectFailureMessage);
		}

		if (options.expected_current_content != nullptr) {
			auto expected = config_utils_internal::CheckExpectedContent(
			    config_path, *options.expected_current_content, options.validation_root);
			if (expected.status != ConfigWriteStatus::kOk) {
				return expected;
			}
		}
		const auto install_result =
		    config_utils_internal::InstallConfigContent(config_path, content, options.sync_parent);
		if (!install_result.has_value()) {
			return Failure(ConfigWriteStatus::kStagingFailed, "Failed to stage updated config");
		}
		if (*install_result == AtomicFileCommitResult::kCommittedSyncFailed) {
			return Failure(ConfigWriteStatus::kDurabilityUncertain,
			               "Config was installed, but its directory could not be synced; verify "
			               "state before retrying");
		}
		if (!AtomicFileCommitIsDurable(*install_result)) {
			return Failure(ConfigWriteStatus::kInstallFailed, kEditedConfigInstallFailedMessage);
		}
		return {.status = ConfigWriteStatus::kOk};
	}

	auto UpdateConfigValue(const std::filesystem::path &config_path,
	                       const ConfigValueUpdate &update, const ConfigUpdateOptions &options)
	    -> ConfigWriteResult {
		if (!config_utils_internal::IsSafeIniScalarValue(update.value)) {
			return Failure(ConfigWriteStatus::kInvalidRequest,
			               "Config values must be single-line scalars and cannot start with [");
		}
		if (!config_utils_internal::ReadsBackUnchanged(update.value)) {
			return Failure(ConfigWriteStatus::kInvalidRequest,
			               "Config values cannot start or end with whitespace, start with ;, or "
			               "contain an inline comment (whitespace followed by ;)");
		}

		auto initial_security =
		    CheckSecureConfigPath(config_path, DefaultSecureOwnerUid(), options.validation_root);
		if (!initial_security.ok) {
			return Failure(ConfigWriteStatus::kInsecurePath,
			               std::move(initial_security.error_message));
		}

		ScopedFileLock config_lock;
		if (options.lock) {
			auto acquired = config_utils_internal::AcquireConfigLock(config_path);
			if (!acquired.has_value()) {
				return Failure(ConfigWriteStatus::kLockFailed, std::move(acquired.error()));
			}
			config_lock = std::move(*acquired);
		}

		auto opened =
		    OpenSecureConfigForRead(config_path, DefaultSecureOwnerUid(), options.validation_root);
		if (!opened.fd.Valid()) {
			return Failure(ConfigWriteStatus::kReadFailed,
			               config_utils_internal::kConfigFileOpenFailureMessage);
		}
		if (!opened.security.ok) {
			return Failure(ConfigWriteStatus::kInsecurePath,
			               std::move(opened.security.error_message));
		}

		const auto current_content = ReadConfigFromFd(opened.fd.Get());
		opened.fd.Reset();
		if (!current_content.has_value()) {
			return Failure(ConfigWriteStatus::kReadFailed,
			               config_utils_internal::kConfigFileReadFailureMessage);
		}
		auto lines = config_utils_internal::SplitLinesPreserveNewlines(*current_content);

		const auto not_found = [&update] -> ConfigWriteResult {
			return Failure(ConfigWriteStatus::kInvalidRequest,
			               "Could not find a \"" + update.key + "\" config option to set");
		};
		const auto schema = config_schema::RuntimeConfigOptions();
		const auto option = std::ranges::find_if(schema, [&](const auto &candidate) -> auto {
			return candidate.key == update.key;
		});
		if (option == schema.end()) {
			return not_found();
		}
		const auto replaced = config_utils_internal::ReplaceLineValue(
		    lines, {.section = option->section, .key = update.key, .value = update.value});
		switch (replaced.result) {
			case config_utils_internal::ConfigLineReplaceResult::kReplaced:
				break;
			case config_utils_internal::ConfigLineReplaceResult::kNotFound:
				return not_found();
			case config_utils_internal::ConfigLineReplaceResult::kDuplicate:
				return Failure(ConfigWriteStatus::kInvalidRequest,
				               "Config option \"" + update.key +
				                   "\" appears more than once in section [" +
				                   std::string(option->section) + "]");
			case config_utils_internal::ConfigLineReplaceResult::kAmbiguousContinuation:
				return Failure(ConfigWriteStatus::kInvalidRequest,
				               "Config line " + std::to_string(replaced.line) +
				                   " is indented, so updating \"" + update.key +
				                   "\" could drop it or change what it belongs to; edit that line, "
				                   "then retry");
		}

		const auto updated_content = config_utils_internal::JoinLines(lines);
		if (options.validate_runtime) {
			if (auto validation = ValidateConfigContent(updated_content);
			    validation.status != ConfigValidationStatus::kOk) {
				// A config that was valid before the update fails because of the requested value.
				const bool request_at_fault =
				    ValidateConfigContent(*current_content).status == ConfigValidationStatus::kOk;
				return Failure(request_at_fault ? ConfigWriteStatus::kInvalidRequest
				                                : ConfigWriteStatus::kInvalidConfig,
				               std::move(validation.message));
			}
		}

		// The lock above is still held, and the content was validated above.
		auto result =
		    ReplaceConfigContentAtomically(config_path, updated_content,
			                               {
		                                       .lock                     = false,
		                                       .validate_runtime         = false,
		                                       .expected_current_content = &*current_content,
		                                       .validation_root          = options.validation_root,
		                                   });
		if (result.status == ConfigWriteStatus::kInstallFailed) {
			result.message = "Failed to update config file";
		}
		return result;
	}

}  // namespace howdy::native
