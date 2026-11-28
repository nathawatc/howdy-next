#include "readiness/internal.hpp"
#include "storage/user_model_path.hpp"
#include "storage/user_model_status.hpp"
#include "storage/user_model_store.hpp"
#include "storage/user_model_types.hpp"
#include "support/error_message.hpp"
#include "support/file_security.hpp"
#include "support/scoped_fd.hpp"
#include "support/user_names.hpp"

#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

#include <fcntl.h>

#include <sys/stat.h>
#include <sys/types.h>

namespace howdy::native::user_model_store_internal {

	namespace {

		auto ReadinessFailure(UserModelStatus status, std::string message,
		                      std::filesystem::path path = {}) -> UserModelReadinessResult {
			return UserModelReadinessResult{
			    .status        = status,
			    .error_message = std::move(message),
			    .path          = std::move(path),
			};
		}

	}  // namespace

	auto WithErrno(std::string message, int error_number) -> std::string {
		return std::move(message) + " (" + SystemErrorMessage(error_number) + ")";
	}

	// O_NOFOLLOW fails a symlink with ELOOP, a directory cannot be opened for writing, and a
	// socket cannot be opened at all (ENXIO).
	auto OpenErrorMeansNotRegularFile(int error_number) -> bool {
		return error_number == ELOOP || error_number == EISDIR || error_number == ENXIO;
	}

	auto NotRegularFileMessage(std::string_view label, const std::filesystem::path &path)
	    -> std::string {
		return std::string(label) + " must be a regular file: " + path.string();
	}

	auto UserModelInspectionFailedMessage(const std::filesystem::path &path, int error_number)
	    -> std::string {
		return WithErrno(std::string(kUserModelFileInspectionFailedMessage) + ": " + path.string(),
		                 error_number);
	}

	auto SnapshotFromStat(const struct stat &st) -> UserModelFileSnapshot {
		return UserModelFileSnapshot{
		    .dev            = static_cast<std::uint64_t>(st.st_dev),
		    .inode          = static_cast<std::uint64_t>(st.st_ino),
		    .size           = static_cast<std::uintmax_t>(st.st_size),
		    .mtime_seconds  = static_cast<std::int64_t>(st.st_mtim.tv_sec),
		    .mtime_nanosecs = static_cast<std::int64_t>(st.st_mtim.tv_nsec),
		    .ctime_seconds  = static_cast<std::int64_t>(st.st_ctim.tv_sec),
		    .ctime_nanosecs = static_cast<std::int64_t>(st.st_ctim.tv_nsec),
		};
	}

	auto ValidateOpenedModelFd(int fd, const std::filesystem::path &path, uid_t owner_uid,
	                           EmptyModelPolicy empty_policy) -> ValidatedModelFd {
		struct stat opened_file{};
		if (fd < 0 || fstat(fd, &opened_file) != 0) {
			const int error_number = fd < 0 ? EBADF : errno;
			return {
			    .status        = UserModelStatus::kParseError,
			    .error_message = UserModelInspectionFailedMessage(path, error_number),
			};
		}
		const auto security = CheckSecurePathStat(opened_file, SecurePathKind::kRegularFile, path,
		                                          kUserModelFileLabel, owner_uid);
		if (!security.ok) {
			return {
			    .status        = UserModelStatus::kInsecurePath,
			    .error_message = security.error_message,
			};
		}
		// No writer leaves an empty model: a first model is linked in whole at commit, and
		// commits never truncate. An empty file is damaged storage, such as a placeholder an
		// older version left behind, and is reported by name rather than as unparsable JSON.
		if (opened_file.st_size == 0 && empty_policy == EmptyModelPolicy::kReject) {
			return {
			    .status        = UserModelStatus::kParseError,
			    .error_message = std::string(kUserModelFileEmptyMessage) + path.string(),
			};
		}
		return {
		    .status    = UserModelStatus::kOk,
		    .file_stat = opened_file,
		};
	}

	auto CheckReadiness(const UserModelStoreConfig &config, const std::string &user)
	    -> UserModelReadinessResult {
		const auto model_path = ResolveUserModelPath(config.models_dir, user);
		if (!model_path) {
			return ReadinessFailure(UserModelStatus::kInvalidUser, kInvalidUserNameMessage);
		}

		const auto directory_security = CheckSecureRootOwnedDirectoryTree(
		    config.models_dir, kUserModelsDirectoryLabel, config.owner_uid, config.validation_root);
		if (!directory_security.ok) {
			if (directory_security.error_code == ENOENT) {
				return ReadinessFailure(UserModelStatus::kNoModelDirectory, {}, *model_path);
			}
			if (directory_security.error_code != 0) {
				return ReadinessFailure(UserModelStatus::kParseError,
				                        directory_security.error_message, *model_path);
			}
			return ReadinessFailure(UserModelStatus::kInsecurePath,
			                        directory_security.error_message, *model_path);
		}

		const auto file_security =
		    CheckSecureRootOwnedFile(*model_path, kUserModelFileLabel, config.owner_uid);
		if (!file_security.ok) {
			if (file_security.error_code == ENOENT) {
				return ReadinessFailure(UserModelStatus::kNoModel, {}, *model_path);
			}
			if (file_security.error_code != 0) {
				return ReadinessFailure(UserModelStatus::kParseError, file_security.error_message,
				                        *model_path);
			}
			return ReadinessFailure(UserModelStatus::kInsecurePath, file_security.error_message,
			                        *model_path);
		}

		return UserModelReadinessResult{
		    .status = UserModelStatus::kOk,
		    .path   = *model_path,
		};
	}

	auto OpenRuntimeDescriptor(const UserModelStoreConfig &config, const std::string &user)
	    -> UserModelDescriptorResult {
		const auto readiness = CheckReadiness(config, user);
		const auto outcome   = Classify(readiness.status);
		if (outcome == UserModelOutcome::kAbsent) {
			return {.status = UserModelDescriptorStatus::kMissing};
		}
		if (readiness.status == UserModelStatus::kInvalidUser) {
			return {
			    .status        = UserModelDescriptorStatus::kInvalidUser,
			    .error_message = std::string(kInvalidUserNameMessage),
			};
		}
		if (outcome != UserModelOutcome::kOk) {
			return {
			    .status        = UserModelDescriptorStatus::kInvalidStorage,
			    .error_message = readiness.error_message.empty()
				                     ? "Failed to validate user model file"
				                     : readiness.error_message,
			};
		}

		constexpr int open_flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
		ScopedFd      model(open(readiness.path.c_str(), open_flags));
		if (!model.Valid()) {
			const int error_number = errno;
			// Removed between the readiness walk and the open: as missing as before the walk.
			if (error_number == ENOENT) {
				return {.status = UserModelDescriptorStatus::kMissing};
			}
			return {
			    .status        = UserModelDescriptorStatus::kInvalidStorage,
			    .error_message = WithErrno(
			        "Failed to open user model file: " + readiness.path.string(), error_number),
			};
		}
		// The walk checked the path; this checks the file the descriptor actually names.
		auto validated = ValidateOpenedModelFd(model.Get(), readiness.path, config.owner_uid,
		                                       EmptyModelPolicy::kReject);
		if (validated.status != UserModelStatus::kOk) {
			return {
			    .status        = UserModelDescriptorStatus::kInvalidStorage,
			    .error_message = std::move(validated.error_message),
			};
		}
		return {
		    .status = UserModelDescriptorStatus::kReady,
		    .fd     = std::move(model),
		};
	}

	auto ResolvePath(const UserModelStoreConfig &config, const std::string &user)
	    -> UserModelPathResult {
		const auto readiness = CheckReadiness(config, user);
		return UserModelPathResult{
		    .status        = readiness.status,
		    .error_message = readiness.error_message,
		    .path          = readiness.path,
		};
	}

}  // namespace howdy::native::user_model_store_internal

// The constructor and the runtime descriptor open live with readiness, apart from the codec and
// the writers, so root PAM links only this unit (see src/storage/user_model_store/AGENTS.md).
namespace howdy::native {

	UserModelStore::UserModelStore(UserModelStoreConfig config)
	    : config_(std::move(config)) {}

	auto UserModelStore::OpenRuntimeDescriptor(const std::string &user) const
	    -> UserModelDescriptorResult {
		return user_model_store_internal::OpenRuntimeDescriptor(config_, user);
	}

}  // namespace howdy::native
