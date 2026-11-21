#include "config/runtime_paths.hpp"
#include "storage/user_model_limits.hpp"
#include "storage/user_model_path.hpp"
#include "storage/user_model_readiness.hpp"
#include "storage/user_model_status.hpp"
#include "storage/user_model_store/test_hooks.hpp"
#include "storage/user_model_types.hpp"
#include "support/atomic_files.hpp"
#include "support/bit_flags.hpp"
#include "support/eintr.hpp"
#include "support/error_message.hpp"
#include "support/fd_io.hpp"
#include "support/file_lock.hpp"
#include "support/file_security.hpp"
#include "support/file_security/validation_root.hpp"
#include "support/scoped_fd.hpp"
#include "support/user_names.hpp"
#include "user_model_codec/internal.hpp"
#include "user_model_store/internal.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>

namespace howdy::native {

	namespace {

		constexpr mode_t kUserModelsDirMode = S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IXGRP;
		constexpr mode_t kUserModelFileMode = S_IRUSR | S_IWUSR;
		// Root-only: a user who can open the lock file can hold it and stall every writer.
		constexpr mode_t           kUserModelLockFileMode  = S_IRUSR | S_IWUSR;
		constexpr std::string_view kUserModelTempPrefix    = ".howdy-user-model-";
		constexpr std::string_view kUserModelsLockFileName = ".howdy-writers.lock";
		// Where a removal moves the model before unlinking it. A fixed short name, not one
		// derived from the model's, so a model name near NAME_MAX still fits; writers hold the
		// namespace lock and a removal frees the name first, so only one removal uses it.
		constexpr std::string_view kUserModelRemovalName = ".howdy-user-model-removed";
		constexpr auto             kUserModelFileTooLargeMessage =
		    "User model file is too large or unreadable: ";
		constexpr auto kUserModelFileReadFailedMessage = "Failed to read user model file: ";
		constexpr auto kOpenedModelFileInsecureMessage =
		    "Opened user model file failed security validation: ";

		auto StoreListFailure(UserModelStatus status, std::string message) -> UserModelListResult {
			return UserModelListResult{
			    .status        = status,
			    .error_message = std::move(message),
			};
		}

		auto InspectFailure(UserModelStatus status, std::string message) -> UserModelInspectResult {
			return UserModelInspectResult{
			    .status        = status,
			    .error_message = std::move(message),
			};
		}

		auto OpenedModelFileIsSecure(const struct stat &opened_file, std::optional<uid_t> owner_uid)
		    -> bool {
			return S_ISREG(opened_file.st_mode) &&
			       (!owner_uid.has_value() || opened_file.st_uid == *owner_uid) &&
			       !HasAnyBits(opened_file.st_mode, S_IWGRP, S_IWOTH) && opened_file.st_nlink == 1;
		}

		auto SnapshotModelFile(const std::filesystem::path &path)
		    -> std::optional<UserModelFileSnapshot> {
			struct stat st{};
			if (stat(path.c_str(), &st) != 0) {
				return std::nullopt;
			}
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

		auto SnapshotsMatch(const UserModelFileSnapshot &left, const UserModelFileSnapshot &right)
		    -> bool {
			return left.dev == right.dev && left.inode == right.inode && left.size == right.size &&
			       left.mtime_seconds == right.mtime_seconds &&
			       left.mtime_nanosecs == right.mtime_nanosecs &&
			       left.ctime_seconds == right.ctime_seconds &&
			       left.ctime_nanosecs == right.ctime_nanosecs;
		}

		auto SameFileIdentity(const struct stat &left, const struct stat &right) -> bool {
			return left.st_dev == right.st_dev && left.st_ino == right.st_ino;
		}

		// Whether `path` still names the opened `fd`, or, with no file open (-1), is still free.
		// A symlink counts as something at the path. nullopt, with errno set, when lstat or
		// fstat cannot tell.
		auto PathStillMatches(const std::filesystem::path &path, int fd) -> std::optional<bool> {
			struct stat path_stat{};
			if (lstat(path.c_str(), &path_stat) != 0) {
				if (errno != ENOENT) {
					return std::nullopt;
				}
				return fd < 0;
			}
			if (fd < 0) {
				return false;
			}
			struct stat fd_stat{};
			if (fstat(fd, &fd_stat) != 0) {
				return std::nullopt;
			}
			return SameFileIdentity(fd_stat, path_stat);
		}

		// The commit result when `path` may no longer name `fd`: changed when it does not, and
		// not committed when that cannot be told. nullopt when it still does.
		auto IdentityFailure(const std::filesystem::path &path, int fd)
		    -> std::optional<AtomicFileCommitResult> {
			const auto unchanged = PathStillMatches(path, fd);
			if (!unchanged.has_value()) {
				return AtomicFileCommitResult::kNotCommitted;
			}
			if (!*unchanged) {
				return AtomicFileCommitResult::kDestinationChanged;
			}
			return std::nullopt;
		}

		// A test hook's injected errno stands in for the rename.
		auto RenameUnlessInjected(const std::filesystem::path &from,
		                          const std::filesystem::path &to, AtomicRenameMode mode,
		                          std::optional<int> injected_errno) -> AtomicRenameResult {
			if (injected_errno.has_value()) {
				return AtomicRenameResultForErrno(*injected_errno);
			}
			return AtomicRename(from, to, mode);
		}

		// The RENAME_NOREPLACE that links a first model in, for InstallStagedFile.
		auto LinkFirstModel(const std::filesystem::path &from, const std::filesystem::path &to,
		                    AtomicRenameMode mode) -> AtomicRenameResult {
			return RenameUnlessInjected(from, to, mode,
			                            user_model_store_test_hooks::Current().create_errno);
		}

		auto SyncModelsDirectory(const std::filesystem::path &path) -> bool {
			return !user_model_store_test_hooks::Current().fail_parent_sync &&
			       SyncParentDirectory(path);
		}

		auto CleanupStaleWriteArtifacts(const std::filesystem::path &path) -> bool {
			std::error_code ec;
			for (std::filesystem::directory_iterator entries(path.parent_path(), ec), end;
			     !ec && entries != end; entries.increment(ec)) {
				const auto filename = entries->path().filename().string();
				if (filename.starts_with(kUserModelTempPrefix) &&
				    unlink(entries->path().c_str()) != 0) {
					return false;
				}
			}
			return !ec;
		}

		// Staging refuses a destination that is not a regular file. Such an entry was put at the
		// path since the open, so the model changed. A regular file put there passes staging and
		// is caught at commit instead, so a staging failure with one there has another cause.
		auto StagingFailureResult(const std::filesystem::path &path) -> AtomicFileCommitResult {
			struct stat path_stat{};
			return lstat(path.c_str(), &path_stat) == 0 && !S_ISREG(path_stat.st_mode)
			           ? AtomicFileCommitResult::kDestinationChanged
					   : AtomicFileCommitResult::kNotCommitted;
		}

		// Commits `document` at `path`. With `opened_fd`, the model it names is exchanged for
		// the new one. Without one (-1), no model exists yet and the new one is linked in with
		// RENAME_NOREPLACE, which refuses a path another actor has filled since the open.
		auto WriteModelsAtomically(const std::filesystem::path      &path,
		                           const user_model_codec::Document &document, int opened_fd)
		    -> AtomicFileCommitResult {
			const bool creating   = opened_fd < 0;
			const auto serialized = user_model_codec::SerializeDocument(document);
			if (!serialized.has_value() ||
			    serialized->size() > user_model_limits::kMaxUserModelFileBytes ||
			    !CleanupStaleWriteArtifacts(path)) {
				return AtomicFileCommitResult::kNotCommitted;
			}
			// A first model takes the default owner and mode, never those of a file that
			// appeared at the path since the open.
			auto staged = PrepareStagedFile(path, kUserModelTempPrefix, kUserModelFileMode,
			                                creating ? StagedFileMetadataPolicy::kUseDefaultMode
			                                         : StagedFileMetadataPolicy::kPreserveExisting,
			                                StagedFileParentPolicy::kRequireExisting);
			if (!staged.has_value()) {
				return StagingFailureResult(path);
			}
			auto const &hooks = user_model_store_test_hooks::Current();
			if (hooks.fail_write || !WriteAllToFd(staged->fd.Get(), *serialized)) {
				CleanupStagedFile(*staged);
				return AtomicFileCommitResult::kNotCommitted;
			}
			if (hooks.fail_fsync || !SyncFd(staged->fd.Get())) {
				CleanupStagedFile(*staged);
				return AtomicFileCommitResult::kNotCommitted;
			}
			if (hooks.before_write_commit) {
				hooks.before_write_commit(path);
			}
			if (!creating) {
				if (const auto failure = IdentityFailure(path, opened_fd); failure.has_value()) {
					CleanupStagedFile(*staged);
					return *failure;
				}
			}
			if (hooks.after_write_identity_check) {
				hooks.after_write_identity_check(path);
			}
			if (creating) {
				return InstallStagedFile(*staged, path, SyncModelsDirectory,
				                         AtomicFileInstallPolicy::kNoReplaceExisting,
				                         LinkFirstModel);
			}
			if (!staged->fd.Close()) {
				CleanupStagedFile(*staged);
				return AtomicFileCommitResult::kNotCommitted;
			}
			const auto exchange_result = RenameUnlessInjected(
			    staged->path, path, AtomicRenameMode::kExchange, hooks.exchange_errno);
			if (exchange_result != AtomicRenameResult::kSuccess) {
				// A model removed after the identity check fails the exchange (ENOENT); report
				// that as a changed model, not a write failure. A replaced model does not fail
				// it, and is caught by the check after the exchange.
				const auto failure = exchange_result == AtomicRenameResult::kFailed
				                         ? IdentityFailure(path, opened_fd)
				                         : std::nullopt;
				CleanupStagedFile(*staged);
				if (failure == AtomicFileCommitResult::kDestinationChanged) {
					return *failure;
				}
				return FailedAtomicRenameCommitResult(exchange_result);
			}
			// The exchange must have swapped out the opened model. If another actor replaced it
			// after the identity check, exchange back and report the model changed.
			if (const auto failure = IdentityFailure(staged->path, opened_fd);
			    failure.has_value()) {
				if (RenameUnlessInjected(staged->path, path, AtomicRenameMode::kExchange,
				                         hooks.rollback_exchange_errno) ==
				    AtomicRenameResult::kSuccess) {
					CleanupStagedFile(*staged);
					return *failure;
				}
				staged->path.clear();
				return AtomicFileCommitResult::kStateUncertain;
			}

			// Exchange commits the canonical write; old-file removal is cleanup only.
			std::error_code ec;
			if (!hooks.fail_write_cleanup) {
				std::filesystem::remove(staged->path, ec);
			}
			staged->path.clear();
			return SyncModelsDirectory(path) ? AtomicFileCommitResult::kCommitted
			                                 : AtomicFileCommitResult::kCommittedSyncFailed;
		}

		// Returns a file a removal moved aside to `path`, then syncs the directory so a crash
		// cannot leave it under the staged name the next write removes. `result` once both
		// are done; the state is uncertain when either fails, since the move aside is the
		// commit and may be what reaches the disk.
		auto MoveAsideBack(const std::filesystem::path &aside, const std::filesystem::path &path,
		                   AtomicFileCommitResult result) -> AtomicFileCommitResult {
			if (AtomicRename(aside, path, AtomicRenameMode::kNoReplace) !=
			        AtomicRenameResult::kSuccess ||
			    !SyncModelsDirectory(path)) {
				return AtomicFileCommitResult::kStateUncertain;
			}
			return result;
		}

		// Removes the model `fd` names. The model is moved aside first and checked there, so
		// what gets unlinked is known to be the opened model: a file another actor put at the
		// path after the identity check is moved back and reported as a changed model, as the
		// exchange's own check does for an update. The move aside is the commit; a crash after
		// it leaves only a staged-prefix file that the next write removes.
		auto RemoveOpenedFile(const std::filesystem::path &path, int fd) -> AtomicFileCommitResult {
			if (fd < 0) {
				return AtomicFileCommitResult::kNotCommitted;
			}
			if (const auto failure = IdentityFailure(path, fd); failure.has_value()) {
				return *failure;
			}
			auto const &hooks = user_model_store_test_hooks::Current();
			if (hooks.before_delete_commit) {
				hooks.before_delete_commit(path);
			}
			static_assert(kUserModelRemovalName.starts_with(kUserModelTempPrefix),
			              "the next write must remove a removal a crash left behind");
			const auto aside = path.parent_path() / kUserModelRemovalName;
			// Free the aside name, which only a removal a crash interrupted can still hold.
			if (unlink(aside.c_str()) != 0 && errno != ENOENT) {
				return AtomicFileCommitResult::kNotCommitted;
			}
			const auto move_result = AtomicRename(path, aside, AtomicRenameMode::kNoReplace);
			if (move_result != AtomicRenameResult::kSuccess) {
				// Removed or replaced since the identity check: the model changed.
				if (const auto failure = IdentityFailure(path, fd);
				    failure == AtomicFileCommitResult::kDestinationChanged) {
					return *failure;
				}
				// The aside name taken (EEXIST) leaves the model untouched; it did not change.
				return move_result == AtomicRenameResult::kDestinationExists
				           ? AtomicFileCommitResult::kNotCommitted
						   : FailedAtomicRenameCommitResult(move_result);
			}
			if (const auto failure = IdentityFailure(aside, fd); failure.has_value()) {
				return MoveAsideBack(aside, path, *failure);
			}
			// Unlink the opened model. If that fails, move it back rather than report a removal
			// while its data stays on disk under a staged name.
			std::error_code ec;
			if (hooks.fail_delete_unlink) {
				ec = std::make_error_code(std::errc::permission_denied);
			} else {
				std::filesystem::remove(aside, ec);
			}
			if (ec) {
				return MoveAsideBack(aside, path, AtomicFileCommitResult::kNotCommitted);
			}
			return SyncModelsDirectory(path) ? AtomicFileCommitResult::kCommitted
			                                 : AtomicFileCommitResult::kCommittedSyncFailed;
		}

		// The namespace lock and the model, or why a writer could not get them.
		struct OpenedUserModelFile {
			UserModelStatus status = UserModelStatus::kOk;
			std::string     error_message;
			ScopedFileLock  namespace_lock;
			// Not valid when no model exists and the caller allowed that.
			ScopedFd file;
		};

		auto OpenFailure(UserModelStatus status, std::string message) -> OpenedUserModelFile {
			return OpenedUserModelFile{
			    .status        = status,
			    .error_message = std::move(message),
			};
		}

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

		// The writers' lock file, inside the models directory, which writers must be able to
		// write anyway. User names cannot start with a dot, so it never names a model.
		auto ModelNamespaceLockPath(const std::filesystem::path &model_path)
		    -> std::filesystem::path {
			return model_path.parent_path() / kUserModelsLockFileName;
		}

		auto LockModelNamespace(const std::filesystem::path &model_path) -> OpenedUserModelFile {
			// Serializes cooperating UserModelStore writers only. Parent-directory write
			// permission still allows uncooperative actors to rename or unlink entries.
			// Writers lock a root-only file, never the models directory or a model file: flock
			// needs only a readable descriptor, and users can open a 0755 models directory and
			// get a read-only descriptor to their own model from the auth helper, so a lock on
			// either would let a user stall every writer. The lock file sits in the models
			// directory, which Resolve checked, root-owned and not writable by others, before
			// either caller gets here, so no user can plant or replace it.
			const auto lock_path = ModelNamespaceLockPath(model_path);
			ScopedFd   fd(open(lock_path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC,
			                   kUserModelLockFileMode));
			if (!fd.Valid()) {
				const int error_number = errno;
				if (OpenErrorMeansNotRegularFile(error_number)) {
					return OpenFailure(UserModelStatus::kInsecurePath,
					                   NotRegularFileMessage(kUserModelLockFileLabel, lock_path));
				}
				return OpenFailure(
				    UserModelStatus::kLockFailed,
				    WithErrno("Failed to open user model lock file: " + lock_path.string(),
					          error_number));
			}
			struct stat lock_stat{};
			if (fstat(fd.Get(), &lock_stat) != 0) {
				const int error_number = errno;
				return OpenFailure(
				    UserModelStatus::kLockFailed,
				    WithErrno("Failed to inspect user model lock file: " + lock_path.string(),
					          error_number));
			}
			// Checked before the blocking flock, so a bad lock file fails instead of waiting.
			auto security = CheckSecurePathStat(lock_stat, SecurePathKind::kRegularFile, lock_path,
			                                    kUserModelLockFileLabel, DefaultSecureOwnerUid());
			if (!security.ok) {
				return OpenFailure(UserModelStatus::kInsecurePath,
				                   std::move(security.error_message));
			}
			if (HasAnyBits(lock_stat.st_mode, S_IRWXG, S_IRWXO)) {
				return OpenFailure(
				    UserModelStatus::kInsecurePath,
				    std::string(kUserModelLockFileLabel) +
				        " must not be accessible by group or others: " + lock_path.string());
			}
			if (RetryOnEintr([&] -> int {
				    return flock(fd.Get(), LOCK_EX);
			    }) != 0) {
				const int error_number = errno;
				return OpenFailure(
				    UserModelStatus::kLockFailed,
				    WithErrno("Failed to lock user models: " + lock_path.string(), error_number));
			}

			return OpenedUserModelFile{.namespace_lock = ScopedFileLock(std::move(fd))};
		}

		// Never creates the model: a first model is linked in whole at commit. A model that is
		// missing is no failure when `allow_missing`, and kNoModel otherwise.
		auto LockNamespaceAndOpenModelFile(const std::filesystem::path &path, bool allow_missing)
		    -> OpenedUserModelFile {
			auto opened = LockModelNamespace(path);
			if (opened.status != UserModelStatus::kOk) {
				return opened;
			}
			ScopedFd fd(open(path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW));
			if (!fd.Valid()) {
				const int error_number = errno;
				if (error_number == ENOENT) {
					return allow_missing ? std::move(opened)
					                     : OpenFailure(UserModelStatus::kNoModel, {});
				}
				if (OpenErrorMeansNotRegularFile(error_number)) {
					return OpenFailure(UserModelStatus::kInsecurePath,
					                   NotRegularFileMessage(kUserModelFileLabel, path));
				}
				return OpenFailure(
				    UserModelStatus::kLockFailed,
				    WithErrno("Failed to open user model file: " + path.string(), error_number));
			}
			struct stat opened_file{};
			if (fstat(fd.Get(), &opened_file) != 0) {
				const int error_number = errno;
				// An inspection failure, as RecheckOpenedPath and Resolve report one.
				return OpenFailure(UserModelStatus::kParseError,
				                   UserModelInspectionFailedMessage(path, error_number));
			}
			if (!OpenedModelFileIsSecure(opened_file, DefaultSecureOwnerUid())) {
				return OpenFailure(UserModelStatus::kInsecurePath,
				                   std::string(kOpenedModelFileInsecureMessage) + path.string());
			}
			opened.file = std::move(fd);
			return opened;
		}

		struct PathRecheck {
			UserModelStatus status = UserModelStatus::kOk;
			std::string     error_message;
		};

		// After the lock and the second path check: the path must still name the opened model,
		// or still be free when there was none (-1). Only a path that names another file, or
		// none, is a changed model; any other failure is reported as one to inspect.
		auto RecheckOpenedPath(const std::filesystem::path &path, int fd) -> PathRecheck {
			const auto unchanged = PathStillMatches(path, fd);
			if (!unchanged.has_value()) {
				const int error_number = errno;
				return PathRecheck{
				    .status        = UserModelStatus::kParseError,
				    .error_message = UserModelInspectionFailedMessage(path, error_number),
				};
			}
			if (!*unchanged) {
				return PathRecheck{
				    .status        = UserModelStatus::kModelChanged,
				    .error_message = std::string(kUserModelChangedMessage),
				};
			}
			return {};
		}

	}  // namespace

	namespace user_model_store_test_hooks {

		auto Current() -> Hooks & {
			static Hooks hooks;
			return hooks;
		}

		ScopedHooks::ScopedHooks(Hooks hooks)
		    : previous_(std::move(Current())) {
			Current() = std::move(hooks);
		}

		ScopedHooks::~ScopedHooks() {
			Current() = std::move(previous_);
		}

	}  // namespace user_model_store_test_hooks

	auto UserModelInspectionFailedMessage(const std::filesystem::path &path, int error_number)
	    -> std::string {
		return WithErrno(std::string(kUserModelFileInspectionFailedMessage) + ": " + path.string(),
		                 error_number);
	}

	UserModelStoreTransaction::UserModelStoreTransaction(std::filesystem::path path,
	                                                     ScopedFileLock        namespace_lock,
	                                                     ScopedFd              file)
	    : path_(std::move(path))
	    , namespace_lock_(std::move(namespace_lock))
	    , file_(std::move(file)) {}

	auto UserModelStoreTransaction::Path() const -> const std::filesystem::path & {
		return path_;
	}

	auto UserModelStoreTransaction::Snapshot() const -> std::optional<UserModelFileSnapshot> {
		return SnapshotModelFile(path_);
	}

	auto UserModelStoreTransaction::SnapshotMatches(const UserModelFileSnapshot &expected) const
	    -> std::optional<bool> {
		const auto current = Snapshot();
		if (!current.has_value()) {
			return std::nullopt;
		}
		return SnapshotsMatch(*current, expected);
	}

	auto UserModelStoreTransaction::WriteDocument(const user_model_codec::Document &document) const
	    -> AtomicFileCommitResult {
		// Without the namespace lock (a moved-from transaction), write nothing.
		if (namespace_lock_.Get() < 0) {
			return AtomicFileCommitResult::kNotCommitted;
		}
		// Without a file, no model existed when the transaction began: create one.
		return WriteModelsAtomically(path_, document, file_.Get());
	}

	auto UserModelStoreTransaction::RemoveFile() const -> AtomicFileCommitResult {
		// As in WriteDocument, without the namespace lock, remove nothing.
		if (namespace_lock_.Get() < 0) {
			return AtomicFileCommitResult::kNotCommitted;
		}
		return RemoveOpenedFile(path_, file_.Get());
	}

	auto UserModelStore::Resolve(const std::string &user, bool create_directory,
	                             std::optional<uid_t>                          owner_uid,
	                             const file_security_internal::ValidationRoot &validation_root)
	    -> UserModelStore::UserModelPathResult {
		const auto models_dir = ResolveUserModelsDir();
		if (!create_directory) {
			const auto readiness =
			    CheckUserModelReadiness(models_dir, user, owner_uid, validation_root);
			return UserModelPathResult{
			    .status        = readiness.status,
			    .error_message = readiness.error_message,
			    .path          = readiness.path,
			};
		}

		const auto model_path = ResolveUserModelPath(models_dir, user);
		if (!model_path) {
			return UserModelPathResult{
			    .status        = UserModelStatus::kInvalidUser,
			    .error_message = kInvalidUserNameMessage,
			};
		}

		std::error_code ec;
		if (!std::filesystem::exists(models_dir, ec)) {
			if (ec) {
				return UserModelPathResult{
				    .status = UserModelStatus::kParseError,
				    .error_message =
				        WithErrno("Failed to inspect user models directory: " + models_dir.string(),
						          ec.value()),
				};
			}
			const auto create_result =
			    CreateSecureDirectoryTree(models_dir, kUserModelsDirectoryLabel, kUserModelsDirMode,
				                          0700, owner_uid, validation_root);
			if (!create_result.ok) {
				return UserModelPathResult{
				    .status        = create_result.error_code == 0
					                     ? UserModelStatus::kInsecurePath
					                     : UserModelStatus::kDirectoryCreateFailed,
				    .error_message = create_result.error_code == 0
					                     ? create_result.error_message
					                     : "Failed to create secure user models directory: " +
					                           models_dir.string(),
				};
			}
		}

		const auto directory_security = CheckSecureRootOwnedDirectoryTree(
		    models_dir, kUserModelsDirectoryLabel, owner_uid, validation_root);
		if (!directory_security.ok) {
			return UserModelPathResult{
			    .status        = UserModelStatus::kInsecurePath,
			    .error_message = directory_security.error_message,
			};
		}

		ec.clear();
		const auto model_exists = std::filesystem::exists(*model_path, ec);
		if (ec) {
			return UserModelPathResult{
			    .status        = UserModelStatus::kParseError,
			    .error_message = UserModelInspectionFailedMessage(*model_path, ec.value()),
			};
		}
		if (model_exists) {
			const auto file_security = CheckSecureRootOwnedFileWithDirectory(
			    *model_path, {.directory = kUserModelsDirectoryLabel, .file = kUserModelFileLabel},
			    owner_uid, validation_root);
			if (!file_security.ok) {
				return UserModelPathResult{
				    .status        = UserModelStatus::kInsecurePath,
				    .error_message = file_security.error_message,
				};
			}
		}

		return UserModelPathResult{.path = *model_path};
	}

	auto UserModelStore::InspectRegularFileStatus(const std::filesystem::path &path,
	                                              std::string *message) -> UserModelStatus {
		std::error_code regular_ec;
		if (std::filesystem::is_regular_file(path, regular_ec)) {
			return UserModelStatus::kOk;
		}
		if (regular_ec) {
			std::error_code exists_ec;
			const auto      exists = std::filesystem::exists(path, exists_ec);
			if (!exists && !exists_ec) {
				return UserModelStatus::kNoModel;
			}
			*message = UserModelInspectionFailedMessage(path, exists_ec ? exists_ec.value()
			                                                            : regular_ec.value());
			return UserModelStatus::kParseError;
		}
		return UserModelStatus::kNoModel;
	}

	auto UserModelStore::LoadDocumentFromFd(int fd, const std::filesystem::path &path,
	                                        const UserModelExpectations &expectations,
	                                        std::optional<uid_t>         owner_uid)
	    -> user_model_codec::Document {
		struct stat opened_file{};
		if (fd < 0 || fstat(fd, &opened_file) != 0) {
			const int error_number = fd < 0 ? EBADF : errno;
			return user_model_codec::Document{
			    StoreListFailure(
			        UserModelStatus::kParseError,
			        WithErrno("Failed to inspect opened user model file: " + path.string(),
					          error_number)),
			};
		}
		if (!OpenedModelFileIsSecure(opened_file, owner_uid)) {
			return user_model_codec::Document{
			    StoreListFailure(UserModelStatus::kInsecurePath,
				                 std::string(kOpenedModelFileInsecureMessage) + path.string()),
			};
		}
		// No writer leaves an empty model, so an empty file is damaged storage. Name it, as the
		// auth helper does, rather than report it as unparsable JSON.
		if (opened_file.st_size == 0) {
			return user_model_codec::Document{
			    StoreListFailure(UserModelStatus::kParseError,
				                 std::string(kUserModelFileEmptyMessage) + path.string()),
			};
		}
		if (opened_file.st_size < 0 ||
		    std::cmp_greater(opened_file.st_size, user_model_limits::kMaxUserModelFileBytes)) {
			return user_model_codec::Document(
			    StoreListFailure(UserModelStatus::kOversized,
				                 std::string(kUserModelFileTooLargeMessage) + path.string()));
		}
		// Read positionally from the start: the offset of an inherited descriptor is shared.
		const auto content = ReadFdToStringBounded({
		    .fd            = fd,
		    .max_bytes     = user_model_limits::kMaxUserModelFileBytes + 1,
		    .reserve_bytes = static_cast<std::size_t>(opened_file.st_size),
		    .offset        = off_t{0},
		});
		if (content.read_error) {
			return user_model_codec::Document{
			    StoreListFailure(UserModelStatus::kParseError,
				                 std::string(kUserModelFileReadFailedMessage) + path.string()),
			};
		}
		if (content.hit_limit) {
			return user_model_codec::Document{
			    StoreListFailure(UserModelStatus::kOversized,
				                 std::string(kUserModelFileTooLargeMessage) + path.string()),
			};
		}

		return user_model_codec::DecodeDocument(content.output, expectations.backend,
		                                        expectations.model, expectations.strict_shape);
	}

	auto UserModelStore::LoadDocumentFromPath(const std::filesystem::path &path,
	                                          const UserModelExpectations &expectations)
	    -> user_model_codec::Document {
		ScopedFd const input(open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
		if (input.Get() < 0) {
			const int error_number = errno;
			if (error_number == ENOENT) {
				return user_model_codec::Document(
				    UserModelListResult{.status = UserModelStatus::kNoModel});
			}
			return user_model_codec::Document{
			    StoreListFailure(
			        UserModelStatus::kParseError,
			        WithErrno("Failed to open user model file: " + path.string(), error_number)),
			};
		}
		return LoadDocumentFromFd(input.Get(), path, expectations, DefaultSecureOwnerUid());
	}

	auto UserModelStore::LoadDocument(const std::string                            &user,
	                                  const UserModelExpectations                  &expectations,
	                                  std::optional<uid_t>                          owner_uid,
	                                  const file_security_internal::ValidationRoot &validation_root)
	    -> user_model_codec::Document {
		const auto path_result = Resolve(user, false, owner_uid, validation_root);
		if (path_result.status != UserModelStatus::kOk) {
			return user_model_codec::Document(
			    StoreListFailure(path_result.status, path_result.error_message));
		}
		return LoadDocumentFromPath(path_result.path, expectations);
	}

	auto UserModelStore::Inspect(const std::string                            &user,
	                             const file_security_internal::ValidationRoot &validation_root)
	    -> UserModelInspectResult {
		const auto path_result = Resolve(user, false, DefaultSecureOwnerUid(), validation_root);
		if (path_result.status != UserModelStatus::kOk) {
			return InspectFailure(path_result.status, path_result.error_message);
		}
		std::string regular_error;
		const auto  regular_status = InspectRegularFileStatus(path_result.path, &regular_error);
		if (regular_status != UserModelStatus::kOk) {
			return InspectFailure(regular_status, regular_error);
		}
		const auto current_snapshot = SnapshotModelFile(path_result.path);
		if (!current_snapshot.has_value()) {
			const int error_number = errno;
			// Removed since the regular-file check: no model, as that check would now report.
			if (error_number == ENOENT) {
				return InspectFailure(UserModelStatus::kNoModel, {});
			}
			return InspectFailure(UserModelStatus::kParseError,
			                      UserModelInspectionFailedMessage(path_result.path, error_number));
		}
		return UserModelInspectResult{
		    .status   = UserModelStatus::kOk,
		    .snapshot = current_snapshot,
		};
	}

	auto
	UserModelStore::BeginMutation(const std::string                            &user,
	                              const file_security_internal::ValidationRoot &validation_root)
	    -> UserModelStoreMutationResult {
		const auto path_result = Resolve(user, true, DefaultSecureOwnerUid(), validation_root);
		if (path_result.status != UserModelStatus::kOk) {
			return UserModelStoreMutationResult{
			    .document = user_model_codec::Document(
			        StoreListFailure(path_result.status, path_result.error_message)),
			};
		}
		if (user_model_store_test_hooks::Current().before_lock) {
			user_model_store_test_hooks::Current().before_lock(path_result.path);
		}
		auto opened_model = LockNamespaceAndOpenModelFile(path_result.path, true);
		if (opened_model.status != UserModelStatus::kOk) {
			return UserModelStoreMutationResult{
			    .document = user_model_codec::Document(
			        StoreListFailure(opened_model.status, std::move(opened_model.error_message))),
			};
		}
		if (user_model_store_test_hooks::Current().after_lock_before_revalidate) {
			user_model_store_test_hooks::Current().after_lock_before_revalidate(path_result.path);
		}

		const auto secured_path = Resolve(user, true, DefaultSecureOwnerUid(), validation_root);
		if (secured_path.status != UserModelStatus::kOk) {
			return UserModelStoreMutationResult{
			    .document = user_model_codec::Document(
			        StoreListFailure(secured_path.status, secured_path.error_message)),
			};
		}
		if (auto recheck = RecheckOpenedPath(path_result.path, opened_model.file.Get());
		    recheck.status != UserModelStatus::kOk) {
			return UserModelStoreMutationResult{
			    .document = user_model_codec::Document(
			        StoreListFailure(recheck.status, std::move(recheck.error_message))),
			};
		}
		const bool have_model = opened_model.file.Valid();
		auto document = have_model
		                    ? LoadDocumentFromFd(opened_model.file.Get(), path_result.path,
		                                         {.backend = {}, .model = {}, .strict_shape = true},
		                                         DefaultSecureOwnerUid())
		                    : user_model_codec::Document(
		                          UserModelListResult{.status = UserModelStatus::kNoModel});
		return UserModelStoreMutationResult{
		    .transaction =
		        UserModelStoreTransaction(path_result.path, std::move(opened_model.namespace_lock),
				                          std::move(opened_model.file)),
		    .document = std::move(document),
		};
	}

	auto UserModelStore::LockExisting(const std::string                            &user,
	                                  const file_security_internal::ValidationRoot &validation_root)
	    -> UserModelStoreTransactionResult {
		const auto path_result = Resolve(user, false, DefaultSecureOwnerUid(), validation_root);
		if (path_result.status != UserModelStatus::kOk) {
			return UserModelStoreTransactionResult{
			    .status        = path_result.status,
			    .error_message = path_result.error_message,
			};
		}
		std::string regular_error;
		const auto  regular_status = InspectRegularFileStatus(path_result.path, &regular_error);
		if (regular_status != UserModelStatus::kOk) {
			return UserModelStoreTransactionResult{
			    .status        = regular_status,
			    .error_message = regular_error,
			};
		}
		if (user_model_store_test_hooks::Current().before_lock) {
			user_model_store_test_hooks::Current().before_lock(path_result.path);
		}
		auto opened_model = LockNamespaceAndOpenModelFile(path_result.path, false);
		if (opened_model.status != UserModelStatus::kOk) {
			return UserModelStoreTransactionResult{
			    .status        = opened_model.status,
			    .error_message = std::move(opened_model.error_message),
			};
		}
		if (user_model_store_test_hooks::Current().after_lock_before_revalidate) {
			user_model_store_test_hooks::Current().after_lock_before_revalidate(path_result.path);
		}

		const auto secured_path = Resolve(user, false, DefaultSecureOwnerUid(), validation_root);
		if (secured_path.status != UserModelStatus::kOk) {
			return UserModelStoreTransactionResult{
			    .status        = secured_path.status,
			    .error_message = secured_path.error_message,
			};
		}
		regular_error.clear();
		const auto secured_regular_status =
		    InspectRegularFileStatus(path_result.path, &regular_error);
		if (secured_regular_status != UserModelStatus::kOk) {
			return UserModelStoreTransactionResult{
			    .status        = secured_regular_status,
			    .error_message = regular_error,
			};
		}
		if (auto recheck = RecheckOpenedPath(path_result.path, opened_model.file.Get());
		    recheck.status != UserModelStatus::kOk) {
			return UserModelStoreTransactionResult{
			    .status        = recheck.status,
			    .error_message = std::move(recheck.error_message),
			};
		}
		return UserModelStoreTransactionResult{
		    .status = UserModelStatus::kOk,
		    .transaction =
		        UserModelStoreTransaction(path_result.path, std::move(opened_model.namespace_lock),
				                          std::move(opened_model.file)),
		};
	}

}  // namespace howdy::native
