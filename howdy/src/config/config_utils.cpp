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

		auto fail_with(std::string *error_message, const std::string &message) -> bool {
			if (error_message != nullptr) {
				*error_message = message;
			}
			return false;
		}

	}  // namespace

	auto replace_config_content_atomically(const std::filesystem::path &config_path,
	                                       const std::string &content, std::string *error_message,
	                                       bool lock, bool validate_runtime,
	                                       const std::string    *expected_current_content,
	                                       SyncParentDirectoryFn sync_parent) -> bool {
		using namespace config_utils_internal;

		if (error_message != nullptr) {
			error_message->clear();
		}
		if (content.size() > kMaxConfigFileSize) {
			return fail_with(error_message, kUpdatedConfigTooLargeMessage);
		}

		const auto initial_security = check_secure_config_path(config_path);
		if (!initial_security.ok) {
			return fail_with(error_message, initial_security.error_message);
		}

		ConfigLockGuard config_lock;
		if (lock && !acquire_config_lock(config_lock, config_path)) {
			return fail_with(error_message, kConfigFileLockFailureMessage);
		}

		if (validate_runtime && !validate_config_content(content, error_message)) {
			return false;
		}

		const auto final_security = check_secure_config_path(config_path);
		if (!final_security.ok) {
			return fail_with(error_message, final_security.error_message);
		}

		struct stat current_stat{};
		if (lstat(config_path.c_str(), &current_stat) != 0 || !S_ISREG(current_stat.st_mode)) {
			return fail_with(error_message, kConfigFileInspectFailureMessage);
		}

		if (expected_current_content != nullptr) {
			if (!expected_content_matches(config_path, *expected_current_content, error_message)) {
				return false;
			}
		}
		const auto install_result =
		    install_config_content(config_path, content, current_stat, sync_parent);
		if (install_result == ConfigInstallResult::committed_not_durable) {
			return fail_with(error_message,
			                 "Config was installed, but its directory could not be synced; verify "
			                 "state before retrying");
		}
		if (install_result == ConfigInstallResult::stage_failed) {
			return fail_with(error_message, "Failed to stage updated config");
		}
		if (install_result == ConfigInstallResult::not_committed) {
			return fail_with(error_message, kEditedConfigInstallFailedMessage);
		}
		return true;
	}

	auto update_config_value(const std::filesystem::path &config_path, const std::string &key,
	                         std::string *error_message, const std::string &value, bool lock,
	                         bool validate_runtime) -> bool {
		using namespace config_utils_internal;

		if (!is_safe_ini_scalar_value(value)) {
			return fail_with(error_message,
			                 "Config values must be single-line scalars and cannot start with [");
		}

		const auto initial_security = check_secure_config_path(config_path);
		if (!initial_security.ok) {
			return fail_with(error_message, initial_security.error_message);
		}

		ConfigLockGuard config_lock;
		if (lock && !acquire_config_lock(config_lock, config_path)) {
			return fail_with(error_message, kConfigFileLockFailureMessage);
		}

		ScopedFd fd(open(config_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
		if (fd.get() < 0) {
			return fail_with(error_message, kConfigFileOpenFailureMessage);
		}
		if (config_test_hooks::current()) {
			config_test_hooks::current()();
		}

		const auto security = check_secure_config_fd(fd.get(), config_path);
		if (!security.ok) {
			return fail_with(error_message, security.error_message);
		}

		const auto current_content = read_config_from_fd(fd.get());
		fd.reset();
		if (!current_content.has_value()) {
			return fail_with(error_message, kConfigFileReadFailureMessage);
		}
		auto lines = split_lines_preserve_newlines(*current_content);

		const auto options = config_schema::runtime_config_options();
		const auto option  = std::ranges::find_if(options, [&](const auto &candidate) -> auto {
			return candidate.key == key;
		});
		if (option == options.end()) {
			return fail_with(error_message,
			                 "Could not find a \"" + key + "\" config option to set");
		}
		const auto replace_result =
		    replace_line_value(lines, {.section = option->section, .key = key, .value = value});
		if (replace_result == ConfigLineReplaceResult::duplicate) {
			return fail_with(error_message, "Config option \"" + key +
			                                    "\" appears more than once in section [" +
			                                    std::string(option->section) + "]");
		}
		if (replace_result == ConfigLineReplaceResult::not_found) {
			return fail_with(error_message,
			                 "Could not find a \"" + key + "\" config option to set");
		}

		const auto updated_content = join_lines(lines);
		if (validate_runtime && !validate_config_content(updated_content, error_message)) {
			return false;
		}

		std::string install_error;
		const bool  ok = replace_config_content_atomically(
		    config_path, updated_content, error_message == nullptr ? nullptr : &install_error,
		    false, false, &*current_content);
		if (!ok && error_message != nullptr) {
			*error_message =
			    install_error.empty() || install_error == kEditedConfigInstallFailedMessage
			        ? "Failed to update config file"
			        : install_error;
		}
		return ok;
	}

}  // namespace howdy::native
