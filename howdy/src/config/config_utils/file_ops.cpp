#include "../config_reader/internal.hpp"
#include "../config_validation/internal.hpp"
#include "config/config_limits.hpp"
#include "config/config_utils.hpp"
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

		auto OpenLockFile(const std::filesystem::path &config_path) -> int {
			return open(LockFilePath(config_path).c_str(),
			            O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR);
		}

		auto LockFd(int fd) -> bool {
			while (flock(fd, LOCK_EX) != 0) {
				if (errno != EINTR) {
					return false;
				}
			}
			return true;
		}

		void UnlockFd(int fd) {
			while (flock(fd, LOCK_UN) != 0 && errno == EINTR) {
			}
		}

		auto FailWith(std::string *error_message, const std::string &message) -> bool {
			if (error_message != nullptr) {
				*error_message = message;
			}
			return false;
		}

	}  // namespace

	ConfigLockGuard::~ConfigLockGuard() {
		if (fd >= 0) {
			UnlockFd(fd);
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
				UnlockFd(fd);
				close(fd);
			}
			fd       = other.fd;
			other.fd = -1;
		}
		return *this;
	}

	auto AcquireConfigLock(ConfigLockGuard &guard, const std::filesystem::path &config_path)
	    -> bool {
		guard.fd = OpenLockFile(config_path);
		if (guard.fd >= 0 && LockFd(guard.fd)) {
			return true;
		}
		if (guard.fd >= 0) {
			close(guard.fd);
			guard.fd = -1;
		}
		return false;
	}

	auto ExpectedContentMatches(const std::filesystem::path &config_path,
	                            const std::string &expected, std::string *error_message,
	                            const file_security_internal::ValidationRoot &validation_root)
	    -> bool {
		ScopedFd input_fd(
		    open(config_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
		if (input_fd.Get() < 0) {
			return FailWith(error_message, kConfigFileOpenFailureMessage);
		}
		if (config_test_hooks::Current()) {
			config_test_hooks::Current()();
		}
		const auto security = CheckSecureConfigFd(input_fd.Get(), config_path,
		                                          DefaultSecureOwnerUid(), validation_root);
		if (!security.ok) {
			return FailWith(error_message, security.error_message);
		}
		const auto current_content = ReadConfigFromFd(input_fd.Get());
		if (!current_content.has_value()) {
			return FailWith(error_message, kConfigFileReadFailureMessage);
		}
		if (*current_content != expected) {
			return FailWith(error_message, kStaleEditedConfigMessage);
		}
		return true;
	}

	auto InstallConfigContent(const std::filesystem::path &config_path, const std::string &content,
	                          const struct stat &current_stat, SyncParentDirectoryFn sync_parent)
	    -> ConfigInstallResult {
		std::string temp =
		    (config_path.parent_path() / (std::string(kConfigTempPrefix) + "XXXXXX")).string();
		std::vector<char> writable(temp.begin(), temp.end());
		writable.push_back('\0');
		const int fd = mkostemp(writable.data(), O_CLOEXEC);
		if (fd < 0) {
			return ConfigInstallResult::kStageFailed;
		}

		const std::filesystem::path temp_path(writable.data());
		bool ok = fchown(fd, current_stat.st_uid, current_stat.st_gid) == 0 &&
		          fchmod(fd, current_stat.st_mode & 07777) == 0 && WriteAllToFd(fd, content) &&
		          SyncFd(fd);
		if (close(fd) != 0) {
			ok = false;
		}
		if (!ok) {
			std::error_code ec;
			std::filesystem::remove(temp_path, ec);
			return ConfigInstallResult::kNotCommitted;
		}

		std::error_code ec;
		std::filesystem::rename(temp_path, config_path, ec);
		if (ec) {
			std::filesystem::remove(temp_path, ec);
			return ConfigInstallResult::kNotCommitted;
		}
		return sync_parent != nullptr && sync_parent(config_path)
		           ? ConfigInstallResult::kOk
		           : ConfigInstallResult::kCommittedNotDurable;
	}

}  // namespace howdy::native::config_utils_internal

namespace howdy::native {

	auto ReadConfigLines(const std::filesystem::path &config_path, bool lock,
	                     const file_security_internal::ValidationRoot &validation_root)
	    -> std::vector<std::string> {
		std::vector<std::string> lines;

		config_utils_internal::ConfigLockGuard config_lock;
		if (lock && !config_utils_internal::AcquireConfigLock(config_lock, config_path)) {
			return lines;
		}

		ScopedFd fd(open(config_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
		if (fd.Get() < 0) {
			return lines;
		}
		if (config_test_hooks::Current()) {
			config_test_hooks::Current()();
		}

		const auto security =
		    CheckSecureConfigFd(fd.Get(), config_path, DefaultSecureOwnerUid(), validation_root);
		if (!security.ok) {
			return lines;
		}

		const auto content = ReadConfigFromFd(fd.Get());
		if (content.has_value()) {
			lines = config_utils_internal::SplitLinesPreserveNewlines(*content);
		}
		return lines;
	}

	auto AtomicWriteLines(const std::filesystem::path    &config_path,
	                      const std::vector<std::string> &lines, SyncParentDirectoryFn sync_parent)
	    -> AtomicFileCommitResult {
		using namespace config_utils_internal;

		auto staged = PrepareStagedFile(config_path, kConfigTempPrefix, kDefaultConfigMode);
		if (!staged.has_value()) {
			return AtomicFileCommitResult::kNotCommitted;
		}

		if (!WriteAllToFd(staged->fd.Get(), config_utils_internal::JoinLines(lines))) {
			CleanupStagedFile(*staged);
			return AtomicFileCommitResult::kNotCommitted;
		}
		return InstallStagedFile(*staged, config_path, sync_parent);
	}

	auto ValidateConfigContent(const std::string &content, std::string *error_message) -> bool {
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
		bool                        ok = WriteAllToFd(fd, content);
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

		if (!config.Ok()) {
			if (error_message != nullptr) {
				*error_message = kUpdatedConfigInvalidMessage;
			}
			return false;
		}

		if (const auto validation = ValidateRuntimeConfig(config)) {
			if (error_message != nullptr) {
				*error_message = *validation;
			}
			return false;
		}

		return true;
	}

}  // namespace howdy::native
