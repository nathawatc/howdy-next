#include "../config_reader/internal.hpp"
#include "../config_validation/internal.hpp"
#include "config/config_limits.hpp"
#include "config/config_utils.hpp"
#include "config/test_hooks.hpp"
#include "internal.hpp"
#include "support/atomic_files.hpp"
#include "support/eintr.hpp"
#include "support/fd_io.hpp"
#include "support/file_lock.hpp"
#include "support/file_security.hpp"
#include "support/file_security/validation_root.hpp"
#include "support/scoped_fd.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <utility>

#include <fcntl.h>

#include <sys/file.h>
#include <sys/stat.h>

namespace howdy::native::config_utils_internal {

	namespace {

		constexpr auto kConfigTempPrefix = ".howdy-config-";

		auto FailWith(std::string *error_message, const std::string &message) -> bool {
			if (error_message != nullptr) {
				*error_message = message;
			}
			return false;
		}

	}  // namespace

	auto AcquireConfigLock(ScopedFileLock &guard, const std::filesystem::path &config_path)
	    -> bool {
		ScopedFd fd(open(LockFilePath(config_path).c_str(),
		                 O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR));
		if (!fd.Valid() || RetryOnEintr([&] -> int {
			                   return flock(fd.Get(), LOCK_EX);
		                   }) != 0) {
			guard.Release();
			return false;
		}
		guard = ScopedFileLock(std::move(fd));
		return true;
	}

	auto ExpectedContentMatches(const std::filesystem::path &config_path,
	                            const std::string &expected, std::string *error_message,
	                            const file_security_internal::ValidationRoot &validation_root)
	    -> bool {
		ScopedFd const input_fd(
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
	                          SyncParentDirectoryFn sync_parent)
	    -> std::optional<AtomicFileCommitResult> {
		auto staged = PrepareStagedFile(config_path, kConfigTempPrefix, kDefaultAtomicFileMode,
		                                StagedFileMetadataPolicy::kPreserveExisting,
		                                StagedFileParentPolicy::kRequireExisting);
		if (!staged.has_value()) {
			return std::nullopt;
		}
		if (!WriteAllToFd(staged->fd.Get(), content)) {
			CleanupStagedFile(*staged);
			return AtomicFileCommitResult::kNotCommitted;
		}
		return InstallStagedFile(*staged, config_path, sync_parent);
	}

}  // namespace howdy::native::config_utils_internal

namespace howdy::native {

	auto ValidateConfigContent(const std::string &content, std::string *error_message) -> bool {
		if (content.size() > kMaxConfigFileSize) {
			if (error_message != nullptr) {
				*error_message = config_utils_internal::kUpdatedConfigTooLargeMessage;
			}
			return false;
		}

		ConfigReader const config("<memory>", content);
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
