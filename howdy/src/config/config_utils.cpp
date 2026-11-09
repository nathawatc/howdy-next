#include "config/config_utils.hpp"

#include "config/config_limits.hpp"
#include "config/config_schema.hpp"
#include "config/test_hooks.hpp"
#include "config_document/internal.hpp"
#include "config_utils/internal.hpp"
#include "config_validation/internal.hpp"
#include "support/atomic_files.hpp"
#include "support/file_lock.hpp"
#include "support/file_security.hpp"
#include "support/file_security/validation_root.hpp"
#include "support/scoped_fd.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <fcntl.h>

#include <sys/stat.h>

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

		// The option and typed value a `howdy set key value` request writes, or the reason it is
		// rejected (`value` is then empty).
		struct RequestedEdit {
			const config_schema::Option              *option = nullptr;
			std::optional<config_schema::ConfigValue> value;
			std::string                               error;
		};

		auto Rejected(std::string error) -> RequestedEdit {
			return {.option = nullptr, .value = std::nullopt, .error = std::move(error)};
		}

		auto ResolveRequestedEdit(const std::string &key, const std::string &value)
		    -> RequestedEdit {
			const auto *option = config_schema::RuntimeConfigOptionByKey(key);
			if (option == nullptr) {
				// Unsafe text is reported without echoing it back to the terminal.
				if (!IsSafeConfigText(key)) {
					return Rejected("Config keys must be printable ASCII");
				}
				return Rejected("Could not find a \"" + key + "\" config option to set");
			}
			// An empty value resets a non-string option by writing its schema default, so the key
			// stays in the file; strings store "", which also selects the default.
			auto typed_value = value.empty() && option->type != config_schema::ValueType::kString
			                       ? config_schema::FallbackConfigValue(*option)
			                       : ParseOptionValueText(*option, value);
			if (typed_value.has_value()) {
				return {.option = option, .value = std::move(typed_value), .error = {}};
			}
			// Unsafe text is reported without echoing it back to the terminal.
			if (!IsSafeConfigText(value)) {
				return Rejected("Config values must be printable ASCII");
			}
			return Rejected(InvalidConfigValueMessage(key, value, option->invalid_rule));
		}

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
		if (error_message != nullptr) {
			error_message->clear();
		}
		if (content.size() > kMaxConfigFileSize) {
			return FailWith(error_message, config_utils_internal::kUpdatedConfigTooLargeMessage);
		}

		const auto initial_security =
		    CheckSecureConfigPath(config_path, DefaultSecureOwnerUid(), validation_root);
		if (!initial_security.ok) {
			return FailWith(error_message, initial_security.error_message);
		}

		ScopedFileLock config_lock;
		if (lock && !config_utils_internal::AcquireConfigLock(config_lock, config_path)) {
			return FailWith(error_message, config_utils_internal::kConfigFileLockFailureMessage);
		}

		// Even recovery writes (validate_runtime == false) never install structurally invalid TOML.
		if (!ValidateConfigContent(content, error_message, validate_runtime)) {
			return false;
		}

		const auto final_security =
		    CheckSecureConfigPath(config_path, DefaultSecureOwnerUid(), validation_root);
		if (!final_security.ok) {
			return FailWith(error_message, final_security.error_message);
		}

		struct stat current_stat{};
		if (lstat(config_path.c_str(), &current_stat) != 0 || !S_ISREG(current_stat.st_mode)) {
			return FailWith(error_message, config_utils_internal::kConfigFileInspectFailureMessage);
		}

		if (expected_current_content != nullptr &&
		    !config_utils_internal::ExpectedContentMatches(config_path, *expected_current_content,
		                                                   error_message, validation_root)) {
			return false;
		}
		const auto install_result =
		    config_utils_internal::InstallConfigContent(config_path, content, sync_parent);
		if (!install_result.has_value()) {
			return FailWith(error_message, "Failed to stage updated config");
		}
		if (*install_result == AtomicFileCommitResult::kCommittedSyncFailed) {
			return FailWith(error_message,
			                "Config was installed, but its directory could not be synced; verify "
			                "state before retrying");
		}
		if (!AtomicFileCommitIsDurable(*install_result)) {
			return FailWith(error_message, kEditedConfigInstallFailedMessage);
		}
		return true;
	}

	auto UpdateConfigValue(const std::filesystem::path &config_path, const std::string &key,
	                       std::string *error_message, const std::string &value, bool lock,
	                       bool                                          validate_runtime,
	                       const file_security_internal::ValidationRoot &validation_root) -> bool {
		const auto requested = ResolveRequestedEdit(key, value);
		if (requested.option == nullptr || !requested.value.has_value()) {
			return FailWith(error_message, requested.error);
		}

		const auto initial_security =
		    CheckSecureConfigPath(config_path, DefaultSecureOwnerUid(), validation_root);
		if (!initial_security.ok) {
			return FailWith(error_message, initial_security.error_message);
		}

		ScopedFileLock config_lock;
		if (lock && !config_utils_internal::AcquireConfigLock(config_lock, config_path)) {
			return FailWith(error_message, config_utils_internal::kConfigFileLockFailureMessage);
		}

		ScopedFd fd(open(config_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
		if (fd.Get() < 0) {
			return FailWith(error_message, config_utils_internal::kConfigFileOpenFailureMessage);
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
			return FailWith(error_message, config_utils_internal::kConfigFileReadFailureMessage);
		}
		const auto edited = SetConfigValue(*current_content, *requested.option, *requested.value);
		if (!edited.ok) {
			return FailWith(error_message, "Cannot update config: " + edited.error);
		}

		// ReplaceConfigContentAtomically validates the edited content (ranges only when
		// validate_runtime is set), checks it against the content read above, and installs it.
		std::string install_error;
		const bool  ok = ReplaceConfigContentAtomically(
		    config_path, edited.content, error_message == nullptr ? nullptr : &install_error, false,
		    validate_runtime, &*current_content, SyncParentDirectory, validation_root);
		if (!ok && error_message != nullptr) {
			*error_message =
			    install_error.empty() || install_error == kEditedConfigInstallFailedMessage
			        ? "Failed to update config file"
			        : install_error;
		}
		return ok;
	}

}  // namespace howdy::native
