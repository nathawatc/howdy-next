#include "../config_reader/internal.hpp"
#include "../config_validation/internal.hpp"
#include "config/config_limits.hpp"
#include "config/config_utils.hpp"
#include "internal.hpp"
#include "support/atomic_files.hpp"
#include "support/bit_flags.hpp"
#include "support/eintr.hpp"
#include "support/error_message.hpp"
#include "support/fd_io.hpp"
#include "support/file_lock.hpp"
#include "support/file_security.hpp"
#include "support/file_security/validation_root.hpp"
#include "support/scoped_fd.hpp"

#include <cerrno>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <fcntl.h>

#include <sys/file.h>
#include <sys/stat.h>

namespace howdy::native::config_utils_internal {

	namespace {

		constexpr auto kConfigTempPrefix = ".howdy-config-";
		constexpr auto kLockReplacedMessage =
		    "Config lock file was removed or replaced while waiting for it; retry";

	}  // namespace

	auto AcquireConfigLock(const std::filesystem::path &config_path)
	    -> std::expected<ScopedFileLock, std::string> {
		// Carry the specific rejection so an admin can tell a lock file with the wrong
		// owner/mode apart from an I/O failure; both still fail closed.
		const auto fail = [](std::string_view reason) -> std::unexpected<std::string> {
			return std::unexpected(std::string(kConfigFileLockFailureMessage) + ": " +
			                       std::string(reason));
		};
		const auto lock_path = LockFilePath(config_path);
		ScopedFd   fd(
		    open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR));
		if (!fd.Valid()) {
			return fail(SystemErrorMessageWithNumber(errno));
		}
		struct stat lock_stat{};
		if (fstat(fd.Get(), &lock_stat) != 0) {
			return fail(SystemErrorMessageWithNumber(errno));
		}
		if (lock_stat.st_nlink == 0) {
			return fail(kLockReplacedMessage);
		}
		// Checked before CheckSecurePathStat, whose write-bit rejection lacks the mode and remedy.
		// Read access alone is enough to open the lock and hold flock(LOCK_EX), letting an
		// unprivileged user block every config write, so read bits are rejected too rather
		// than silently repaired. chmod would not revoke a descriptor opened earlier, so the
		// hint is to remove the file and let O_CREAT make a fresh 0600 inode.
		if (S_ISREG(lock_stat.st_mode) && HasAnyBits(lock_stat.st_mode, S_IRWXG, S_IRWXO)) {
			return fail(std::format("Config lock file must not be accessible by group or others "
			                        "(mode {:04o}, expected 0600; remove {} while no howdy config "
			                        "write is running)",
			                        lock_stat.st_mode & 07777U, lock_path.string()));
		}
		const auto security =
		    CheckSecurePathStat(lock_stat, SecurePathKind::kRegularFile, lock_path,
			                    "Config lock file", DefaultSecureOwnerUid());
		if (!security.ok) {
			return fail(security.error_message);
		}
		if (RetryOnEintr([&] -> int {
			    return flock(fd.Get(), LOCK_EX);
		    }) != 0) {
			return fail(SystemErrorMessageWithNumber(errno));
		}
		// The lock serializes writers only while its path still names the locked file: one
		// removed or replaced during the wait leaves the next writer locking another inode.
		struct stat path_stat{};
		if (lstat(lock_path.c_str(), &path_stat) != 0) {
			return fail(errno == ENOENT ? std::string(kLockReplacedMessage)
			                            : SystemErrorMessageWithNumber(errno));
		}
		if (path_stat.st_dev != lock_stat.st_dev || path_stat.st_ino != lock_stat.st_ino) {
			return fail(kLockReplacedMessage);
		}
		return ScopedFileLock(std::move(fd));
	}

	auto CheckExpectedContent(const std::filesystem::path &config_path, const std::string &expected,
	                          const file_security_internal::ValidationRoot &validation_root)
	    -> ConfigWriteResult {
		const auto opened =
		    OpenSecureConfigForRead(config_path, DefaultSecureOwnerUid(), validation_root);
		if (!opened.fd.Valid()) {
			return OpenFailure(opened.open_error);
		}
		if (!opened.security.ok) {
			return PathFailure(opened.security);
		}
		const auto current_content = ReadConfigFromFd(opened.fd.Get());
		if (!current_content.has_value()) {
			return {
			    .status  = ConfigWriteStatus::kReadFailed,
			    .message = kConfigFileReadFailureMessage,
			};
		}
		if (*current_content != expected) {
			return {
			    .status  = ConfigWriteStatus::kStale,
			    .message = kStaleEditedConfigMessage,
			};
		}
		return {.status = ConfigWriteStatus::kOk};
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
		if (!WriteAllToFd(staged->Fd().Get(), content)) {
			return AtomicFileCommitResult::kNotCommitted;
		}
		return InstallStagedFile(*staged, config_path, sync_parent);
	}

}  // namespace howdy::native::config_utils_internal

namespace howdy::native {

	auto ValidateConfigContent(const std::string &content) -> ConfigValidationResult {
		if (content.size() > kMaxConfigFileSize) {
			return {
			    .status  = ConfigValidationStatus::kTooLarge,
			    .message = config_utils_internal::kUpdatedConfigTooLargeMessage,
			};
		}

		ConfigReader const config(content);
		if (config.Damage() != ConfigDamage::kNone) {
			return {
			    .status  = ConfigValidationStatus::kDamaged,
			    .message = std::format("Config {}", ConfigDamageDescription(config.Damage())),
			};
		}
		if (!config.Ok()) {
			return {
			    .status  = ConfigValidationStatus::kUnparsable,
			    .message = config_utils_internal::kUpdatedConfigInvalidMessage,
			};
		}

		if (auto invalid_setting = ValidateRuntimeConfig(config)) {
			return {
			    .status  = ConfigValidationStatus::kInvalidSetting,
			    .message = std::move(*invalid_setting),
			};
		}

		return {.status = ConfigValidationStatus::kOk};
	}

}  // namespace howdy::native
