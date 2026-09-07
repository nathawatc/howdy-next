#include "config/config_limits.hpp"
#include "config/config_reader.hpp"
#include "config/config_utils.hpp"
#include "config/config_validation.hpp"
#include "config/test_hooks.hpp"
#include "internal.hpp"
#include "support/atomic_files.hpp"
#include "support/fd_io.hpp"
#include "support/file_lock.hpp"

#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

#include <sys/file.h>
#include <sys/stat.h>

namespace howdy::native::config_utils_internal {

	namespace {

		constexpr mode_t kDefaultConfigMode              = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
		constexpr auto   kConfigValidationFailureMessage = "Failed to validate updated config";
		constexpr auto   kConfigTempPrefix               = ".howdy-config-";

		auto open_lock_file(const std::filesystem::path &config_path) -> int {
			return open(lock_file_path(config_path).c_str(),
			            O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR);
		}

		auto lock_fd(int fd) -> bool {
			while (flock(fd, LOCK_EX) != 0) {
				if (errno != EINTR) {
					return false;
				}
			}
			return true;
		}

		void unlock_fd(int fd) {
			while (flock(fd, LOCK_UN) != 0 && errno == EINTR) {
			}
		}

		auto fail_with(std::string *error_message, const std::string &message) -> bool {
			if (error_message != nullptr) {
				*error_message = message;
			}
			return false;
		}

	}  // namespace

	ConfigLockGuard::~ConfigLockGuard() {
		if (fd >= 0) {
			unlock_fd(fd);
			close(fd);
		}
	}

	ConfigLockGuard::ConfigLockGuard(ConfigLockGuard &&other) noexcept
	    : fd(other.fd) {
		other.fd = -1;
	}

	auto ConfigLockGuard::operator=(ConfigLockGuard &&other) noexcept -> ConfigLockGuard & {
		if (this != &other) {
			if (fd >= 0) {
				unlock_fd(fd);
				close(fd);
			}
			fd       = other.fd;
			other.fd = -1;
		}
		return *this;
	}

	auto acquire_config_lock(ConfigLockGuard &guard, const std::filesystem::path &config_path)
	    -> bool {
		guard.fd = open_lock_file(config_path);
		if (guard.fd >= 0 && lock_fd(guard.fd)) {
			return true;
		}
		if (guard.fd >= 0) {
			close(guard.fd);
			guard.fd = -1;
		}
		return false;
	}

	auto expected_content_matches(const std::filesystem::path &config_path,
	                              const std::string &expected, std::string *error_message) -> bool {
		ScopedFd input_fd(
		    open(config_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
		if (input_fd.get() < 0) {
			return fail_with(error_message, kConfigFileOpenFailureMessage);
		}
		if (config_test_hooks::current()) {
			config_test_hooks::current()();
		}
		const auto security = check_secure_config_fd(input_fd.get(), config_path);
		if (!security.ok) {
			return fail_with(error_message, security.error_message);
		}
		const auto current_content = read_config_from_fd(input_fd.get());
		if (!current_content.has_value()) {
			return fail_with(error_message, kConfigFileReadFailureMessage);
		}
		if (*current_content != expected) {
			return fail_with(error_message, kStaleEditedConfigMessage);
		}
		return true;
	}

	auto install_config_content(const std::filesystem::path &config_path,
	                            const std::string &content, const struct stat &current_stat,
	                            SyncParentDirectoryFn sync_parent) -> ConfigInstallResult {
		std::string temp =
		    (config_path.parent_path() / (std::string(kConfigTempPrefix) + "XXXXXX")).string();
		std::vector<char> writable(temp.begin(), temp.end());
		writable.push_back('\0');
		const int fd = mkostemp(writable.data(), O_CLOEXEC);
		if (fd < 0) {
			return ConfigInstallResult::stage_failed;
		}

		const std::filesystem::path temp_path(writable.data());
		bool ok = fchown(fd, current_stat.st_uid, current_stat.st_gid) == 0 &&
		          fchmod(fd, current_stat.st_mode & 07777) == 0 && write_all_to_fd(fd, content) &&
		          sync_fd(fd);
		if (close(fd) != 0) {
			ok = false;
		}
		if (!ok) {
			std::error_code ec;
			std::filesystem::remove(temp_path, ec);
			return ConfigInstallResult::not_committed;
		}

		std::error_code ec;
		std::filesystem::rename(temp_path, config_path, ec);
		if (ec) {
			std::filesystem::remove(temp_path, ec);
			return ConfigInstallResult::not_committed;
		}
		return sync_parent != nullptr && sync_parent(config_path)
		           ? ConfigInstallResult::ok
		           : ConfigInstallResult::committed_not_durable;
	}

}  // namespace howdy::native::config_utils_internal

namespace howdy::native {

	auto read_config_lines(const std::filesystem::path &config_path, bool lock)
	    -> std::vector<std::string> {
		std::vector<std::string> lines;

		config_utils_internal::ConfigLockGuard config_lock;
		if (lock && !config_utils_internal::acquire_config_lock(config_lock, config_path)) {
			return lines;
		}

		ScopedFd fd(open(config_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
		if (fd.get() < 0) {
			return lines;
		}
		if (config_test_hooks::current()) {
			config_test_hooks::current()();
		}

		const auto security = check_secure_config_fd(fd.get(), config_path);
		if (!security.ok) {
			return lines;
		}

		const auto content = read_config_from_fd(fd.get());
		if (content.has_value()) {
			lines = config_utils_internal::split_lines_preserve_newlines(*content);
		}
		return lines;
	}

	auto atomic_write_lines(const std::filesystem::path    &config_path,
	                        const std::vector<std::string> &lines,
	                        SyncParentDirectoryFn           sync_parent) -> AtomicFileCommitResult {
		using namespace config_utils_internal;

		auto staged = prepare_staged_file(config_path, kConfigTempPrefix, kDefaultConfigMode);
		if (!staged.has_value()) {
			return AtomicFileCommitResult::kNotCommitted;
		}

		if (!write_all_to_fd(staged->fd.get(), config_utils_internal::join_lines(lines))) {
			cleanup_staged_file(*staged);
			return AtomicFileCommitResult::kNotCommitted;
		}
		return install_staged_file(*staged, config_path, sync_parent);
	}

	auto validate_config_content(const std::string &content, std::string *error_message) -> bool {
		using namespace config_utils_internal;

		if (content.size() > kMaxConfigFileSize) {
			if (error_message != nullptr) {
				*error_message = kUpdatedConfigTooLargeMessage;
			}
			return false;
		}
		const auto        temp_root     = std::filesystem::temp_directory_path();
		std::string       temp_template = (temp_root / "howdy-config-validate-XXXXXX").string();
		std::vector<char> writable(temp_template.begin(), temp_template.end());
		writable.push_back('\0');

		const int fd = mkostemp(writable.data(), O_CLOEXEC);
		if (fd < 0) {
			if (error_message != nullptr) {
				*error_message = kConfigValidationFailureMessage;
			}
			return false;
		}

		const std::filesystem::path temp_path(writable.data());
		bool                        ok = write_all_to_fd(fd, content);
		if (close(fd) != 0) {
			ok = false;
		}

		if (!ok) {
			std::error_code ec;
			std::filesystem::remove(temp_path, ec);
			if (error_message != nullptr) {
				*error_message = kConfigValidationFailureMessage;
			}
			return false;
		}

		ConfigReader    config(temp_path.string());
		std::error_code ec;
		std::filesystem::remove(temp_path, ec);

		if (!config.ok()) {
			if (error_message != nullptr) {
				*error_message = kUpdatedConfigInvalidMessage;
			}
			return false;
		}

		if (const auto validation = validate_runtime_config(config)) {
			if (error_message != nullptr) {
				*error_message = *validation;
			}
			return false;
		}

		return true;
	}

}  // namespace howdy::native
