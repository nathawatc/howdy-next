#include "../config_reader/internal.hpp"
#include "../config_validation/internal.hpp"
#include "config/config_limits.hpp"
#include "config/config_utils.hpp"
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
		const auto opened =
		    OpenSecureConfigForRead(config_path, DefaultSecureOwnerUid(), validation_root);
		if (!opened.fd.Valid()) {
			return FailWith(error_message, kConfigFileOpenFailureMessage);
		}
		if (!opened.security.ok) {
			return FailWith(error_message, opened.security.error_message);
		}
		const auto current_content = ReadConfigFromFd(opened.fd.Get());
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
			return config_utils_internal::FailWith(
			    error_message, config_utils_internal::kUpdatedConfigTooLargeMessage);
		}

		ConfigReader const config("<memory>", content);
		if (!config.Ok()) {
			return config_utils_internal::FailWith(error_message, kUpdatedConfigInvalidMessage);
		}

		if (const auto validation = ValidateRuntimeConfig(config)) {
			return config_utils_internal::FailWith(error_message, *validation);
		}

		return true;
	}

}  // namespace howdy::native
