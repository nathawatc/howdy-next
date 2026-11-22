#include "../user_model_codec/internal.hpp"
#include "internal.hpp"
#include "readiness/internal.hpp"
#include "storage/user_model_limits.hpp"
#include "storage/user_model_path.hpp"
#include "storage/user_model_status.hpp"
#include "storage/user_model_store.hpp"
#include "storage/user_model_store/test_hooks.hpp"
#include "storage/user_model_types.hpp"
#include "support/atomic_files.hpp"
#include "support/bit_flags.hpp"
#include "support/eintr.hpp"
#include "support/fd_io.hpp"
#include "support/file_lock.hpp"
#include "support/file_security.hpp"
#include "support/scoped_fd.hpp"
#include "support/user_names.hpp"

#include <cerrno>
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

namespace howdy::native::user_model_store_internal {

	namespace {

		constexpr mode_t kUserModelFileMode = S_IRUSR | S_IWUSR;
		// Root-only: a user who can open the lock file can hold it and stall every writer.
		constexpr mode_t           kUserModelLockFileMode  = S_IRUSR | S_IWUSR;
		constexpr std::string_view kUserModelTempPrefix    = ".howdy-user-model-";
		constexpr std::string_view kUserModelsLockFileName = ".howdy-writers.lock";
		// Where a removal moves the model before unlinking it. A fixed short name, not one
		// derived from the model's, so a model name near NAME_MAX still fits; writers hold the
		// namespace lock and a removal frees the name first, so only one removal uses it.
		constexpr std::string_view kUserModelRemovalName = ".howdy-user-model-removed";

		auto MutationFailure(UserModelStatus status, std::string message)
		    -> UserModelMutationResult {
			return UserModelMutationResult{
			    .status        = status,
			    .error_message = std::move(message),
			};
		}

		auto ModelChangedFailure() -> UserModelMutationResult {
			return MutationFailure(UserModelStatus::kModelChanged,
			                       std::string(kUserModelChangedMessage));
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

		auto SyncModelsDirectory(const std::filesystem::path              &path,
		                         const user_model_store_test_hooks::Hooks &hooks) -> bool {
			return !hooks.fail_parent_sync && SyncParentDirectory(path);
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

		// Links a first model in with RENAME_NOREPLACE through InstallStagedFile, with the
		// store's hooks standing in for the rename and the parent sync.
		auto CommitFirstModel(StagedFile &staged, const std::filesystem::path &path,
		                      const user_model_store_test_hooks::Hooks &hooks)
		    -> AtomicFileCommitResult {
			return InstallStagedFile(
			    staged, path,
			    [&hooks](const std::filesystem::path &destination) -> bool {
				    return SyncModelsDirectory(destination, hooks);
			    },
			    AtomicFileInstallPolicy::kNoReplaceExisting,
			    [&hooks](const std::filesystem::path &from, const std::filesystem::path &to,
				         AtomicRenameMode mode) -> AtomicRenameResult {
				    return RenameUnlessInjected(from, to, mode, hooks.create_errno);
			    });
		}

		auto ExchangeStagedModel(StagedFile &staged, const std::filesystem::path &path,
		                         int opened_fd, const user_model_store_test_hooks::Hooks &hooks)
		    -> AtomicFileCommitResult {
			const auto exchange_result = RenameUnlessInjected(
			    staged.path, path, AtomicRenameMode::kExchange, hooks.exchange_errno);
			if (exchange_result != AtomicRenameResult::kSuccess) {
				// A model removed after the identity check fails the exchange (ENOENT); report
				// that as a changed model, not a write failure. A replaced model does not fail
				// it, and is caught by the check after the exchange.
				const auto failure = exchange_result == AtomicRenameResult::kFailed
				                         ? IdentityFailure(path, opened_fd)
				                         : std::nullopt;
				CleanupStagedFile(staged);
				if (failure == AtomicFileCommitResult::kDestinationChanged) {
					return *failure;
				}
				return FailedAtomicRenameCommitResult(exchange_result);
			}
			// The exchange must have swapped out the opened model. If another actor replaced it
			// after the identity check, exchange back and report the model changed.
			if (const auto failure = IdentityFailure(staged.path, opened_fd); failure.has_value()) {
				if (RenameUnlessInjected(staged.path, path, AtomicRenameMode::kExchange,
				                         hooks.rollback_exchange_errno) ==
				    AtomicRenameResult::kSuccess) {
					CleanupStagedFile(staged);
					return *failure;
				}
				staged.path.clear();
				return AtomicFileCommitResult::kStateUncertain;
			}

			// Exchange commits the canonical write; old-file removal is cleanup only.
			std::error_code ec;
			if (!hooks.fail_write_cleanup) {
				std::filesystem::remove(staged.path, ec);
			}
			staged.path.clear();
			return SyncModelsDirectory(path, hooks) ? AtomicFileCommitResult::kCommitted
			                                        : AtomicFileCommitResult::kCommittedSyncFailed;
		}

		// Commits `document` at `path`. With `opened_fd`, the model it names is exchanged for
		// the new one. Without one (-1), no model exists yet and the new one is linked in with
		// RENAME_NOREPLACE, which refuses a path another actor has filled since the open.
		auto WriteModelsAtomically(const std::filesystem::path      &path,
		                           const user_model_codec::Document &document, int opened_fd,
		                           const user_model_store_test_hooks::Hooks &hooks)
		    -> AtomicFileCommitResult {
			const bool creating   = opened_fd < 0;
			const auto serialized = user_model_codec::SerializeDocument(document);
			if (!serialized.has_value() ||
			    serialized->size() > user_model_limits::kMaxUserModelFileBytes ||
			    !CleanupStaleWriteArtifacts(path)) {
				return AtomicFileCommitResult::kNotCommitted;
			}
			if (hooks.before_write_staging) {
				hooks.before_write_staging(path);
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
			// Runs on both paths: the last hook before the exchange or the no-replace link.
			if (hooks.after_write_identity_check) {
				hooks.after_write_identity_check(path);
			}
			if (creating) {
				return CommitFirstModel(*staged, path, hooks);
			}
			if (!staged->fd.Close()) {
				CleanupStagedFile(*staged);
				return AtomicFileCommitResult::kNotCommitted;
			}
			return ExchangeStagedModel(*staged, path, opened_fd, hooks);
		}

		// Returns a file a removal moved aside to `path`, then syncs the directory so a crash
		// cannot leave it under the staged name the next write removes. `result` once both
		// are done; the state is uncertain when either fails, since the move aside is the
		// commit and may be what reaches the disk.
		auto MoveAsideBack(const std::filesystem::path &aside, const std::filesystem::path &path,
		                   AtomicFileCommitResult                    result,
		                   const user_model_store_test_hooks::Hooks &hooks)
		    -> AtomicFileCommitResult {
			if (AtomicRename(aside, path, AtomicRenameMode::kNoReplace) !=
			        AtomicRenameResult::kSuccess ||
			    !SyncModelsDirectory(path, hooks)) {
				return AtomicFileCommitResult::kStateUncertain;
			}
			return result;
		}

		// Removes the model `fd` names. The model is moved aside first and checked there, so
		// what gets unlinked is known to be the opened model: a file another actor put at the
		// path after the identity check is moved back and reported as a changed model, as the
		// exchange's own check does for an update. The move aside is the commit; a crash after
		// it leaves only a staged-prefix file that the next write removes.
		auto RemoveOpenedFile(const std::filesystem::path &path, int fd,
		                      const user_model_store_test_hooks::Hooks &hooks)
		    -> AtomicFileCommitResult {
			if (fd < 0) {
				return AtomicFileCommitResult::kNotCommitted;
			}
			if (const auto failure = IdentityFailure(path, fd); failure.has_value()) {
				return *failure;
			}
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
				return MoveAsideBack(aside, path, *failure, hooks);
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
				return MoveAsideBack(aside, path, AtomicFileCommitResult::kNotCommitted, hooks);
			}
			return SyncModelsDirectory(path, hooks) ? AtomicFileCommitResult::kCommitted
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

		auto LockModelNamespace(const std::filesystem::path &model_path, uid_t owner_uid)
		    -> OpenedUserModelFile {
			// Serializes cooperating UserModelStore writers only. Parent-directory write
			// permission still allows uncooperative actors to rename or unlink entries.
			// Writers lock a root-only file, never the models directory or a model file: flock
			// needs only a readable descriptor, and the auth helper gives each user a read-only
			// descriptor to their own model, so a lock on that model would let the user stall
			// every writer. The models directory is 0750, so other users cannot open it, but a
			// lock there would depend on that mode. The 0600 lock file is closed to every user.
			// The lock file sits in the models directory, which the writer's path walk
			// (ResolveWritablePath, which may first create it) checked, root-owned and not
			// writable by others, before LockAndRevalidateForMutation gets here, so no user can
			// plant or replace it.
			const auto lock_path = ModelNamespaceLockPath(model_path);
			ScopedFd   fd(open(lock_path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC,
			                   kUserModelLockFileMode));
			if (!fd.Valid()) {
				const int error_number = errno;
				if (OpenErrorMeansNotRegularFile(error_number)) {
					return OpenFailure(UserModelStatus::kInsecurePath,
					                   NotRegularFileMessage(kUserModelLockFileLabel, lock_path));
				}
				// The models directory the path walk checked is gone: it was removed under us.
				if (error_number == ENOENT || error_number == ENOTDIR) {
					return OpenFailure(UserModelStatus::kModelChanged,
					                   std::string(kUserModelChangedMessage));
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
			                                    kUserModelLockFileLabel, owner_uid);
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

		// The namespace lock serializes writers only while the lock path still names the file this
		// writer locked: a models directory removed and recreated since holds another lock.
		// nullopt, with errno set, when lstat or fstat fails for a reason other than a missing
		// lock path, which is an inspection failure rather than a change.
		auto LockPathStillNamesLock(const std::filesystem::path &model_path, int lock_fd)
		    -> std::optional<bool> {
			const auto  lock_path = ModelNamespaceLockPath(model_path);
			struct stat path_stat{};
			if (lstat(lock_path.c_str(), &path_stat) != 0) {
				return errno == ENOENT || errno == ENOTDIR ? std::optional<bool>(false)
				                                           : std::nullopt;
			}
			struct stat lock_stat{};
			if (fstat(lock_fd, &lock_stat) != 0) {
				return std::nullopt;
			}
			return SameFileIdentity(lock_stat, path_stat);
		}

		// Never creates the model: a first model is linked in whole at commit. A model that is
		// missing is no failure when `allow_missing`, and kNoModel otherwise.
		auto LockNamespaceAndOpenModelFile(const std::filesystem::path &path, uid_t owner_uid,
		                                   bool allow_missing) -> OpenedUserModelFile {
			auto opened = LockModelNamespace(path, owner_uid);
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
				// An inspection failure, as RecheckOpenedPath and ResolvePath report one.
				return OpenFailure(UserModelStatus::kParseError,
				                   UserModelInspectionFailedMessage(path, error_number));
			}
			const auto security = CheckSecurePathStat(opened_file, SecurePathKind::kRegularFile,
			                                          path, kUserModelFileLabel, owner_uid);
			if (!security.ok) {
				return OpenFailure(UserModelStatus::kInsecurePath, security.error_message);
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

		auto CommitFailure(AtomicFileCommitResult result, UserModelStatus failure_status,
		                   std::string failure_message, std::string committed_message,
		                   UserModelEntry entry = {}, bool removed_last = false)
		    -> UserModelMutationResult {
			if (result == AtomicFileCommitResult::kAtomicRenameUnsupported) {
				return MutationFailure(
				    UserModelStatus::kAtomicExchangeUnsupported,
				    "Cannot change user model file: the filesystem or kernel does not support the "
				    "atomic rename (renameat2) it needs; no changes were made");
			}
			// A first model's path taken, or an existing model replaced, since the open.
			if (result == AtomicFileCommitResult::kDestinationExists ||
			    result == AtomicFileCommitResult::kDestinationChanged) {
				return ModelChangedFailure();
			}
			// Either a rollback after a concurrent change failed, or a removal could not
			// durably restore the model it moved aside.
			if (result == AtomicFileCommitResult::kStateUncertain) {
				return UserModelMutationResult{
				    .status        = UserModelStatus::kCommitStateUncertain,
				    .error_message = "User model commit could not be cleanly rolled back; inspect "
				                     "state before retrying",
				};
			}
			if (result == AtomicFileCommitResult::kCommittedSyncFailed) {
				return UserModelMutationResult{
				    .status        = UserModelStatus::kDurabilityUncertain,
				    .error_message = std::move(committed_message),
				    .entry         = std::move(entry),
				    .removed_last  = removed_last,
				};
			}
			return MutationFailure(failure_status, std::move(failure_message));
		}

		class UserModelStoreTransaction {
		public:
			UserModelStoreTransaction(std::filesystem::path path, ScopedFileLock namespace_lock,
			                          ScopedFd file, user_model_store_test_hooks::Hooks hooks)
			    : path_(std::move(path))
			    , namespace_lock_(std::move(namespace_lock))
			    , file_(std::move(file))
			    , hooks_(std::move(hooks)) {}

			UserModelStoreTransaction(const UserModelStoreTransaction &) = delete;
			auto operator=(const UserModelStoreTransaction &)
			    -> UserModelStoreTransaction &                               = delete;
			UserModelStoreTransaction(UserModelStoreTransaction &&) noexcept = default;
			auto operator=(UserModelStoreTransaction &&) noexcept
			    -> UserModelStoreTransaction & = default;
			~UserModelStoreTransaction()       = default;

			[[nodiscard]] auto WriteDocument(const user_model_codec::Document &document) const
			    -> AtomicFileCommitResult {
				if (namespace_lock_.Get() < 0) {
					return AtomicFileCommitResult::kNotCommitted;
				}
				return WriteModelsAtomically(path_, document, file_.Get(), hooks_);
			}

			[[nodiscard]] auto RemoveFile() const -> AtomicFileCommitResult {
				if (namespace_lock_.Get() < 0) {
					return AtomicFileCommitResult::kNotCommitted;
				}
				return RemoveOpenedFile(path_, file_.Get(), hooks_);
			}

		private:
			std::filesystem::path path_;
			ScopedFileLock        namespace_lock_;
			// The model file, held open to check that the path still names it, or not valid when no
			// model existed and a first one is to be linked in. Not locked: the root-only
			// namespace lock serializes writers, and a user holding a descriptor to the file from
			// the auth helper could otherwise stall them with flock.
			ScopedFd                           file_;
			user_model_store_test_hooks::Hooks hooks_;
		};

		auto IsMissingModelStatus(UserModelStatus status) -> bool {
			return Classify(status) == UserModelOutcome::kAbsent;
		}

		auto MapMutationStepFailure(UserModelStatus status, std::string message, bool allow_missing)
		    -> UserModelMutationResult {
			if (!allow_missing && IsMissingModelStatus(status)) {
				return ModelChangedFailure();
			}
			return MutationFailure(status, std::move(message));
		}

		auto VerifyExistingRegularFile(const std::filesystem::path &path)
		    -> std::optional<UserModelMutationResult> {
			std::string regular_error;
			const auto  regular_status = InspectRegularFileStatus(path, &regular_error);
			if (regular_status == UserModelStatus::kOk) {
				return std::nullopt;
			}
			return MapMutationStepFailure(regular_status, std::move(regular_error), false);
		}

		struct LockedMutationTarget {
			std::optional<UserModelMutationResult> failure;
			std::filesystem::path                  path;
			OpenedUserModelFile                    opened;
		};

		constexpr mode_t kUserModelsDirMode = S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IXGRP;

		// A writer's path walk. An append may create the models directory before the lock; every
		// other step uses the read-only ResolvePath.
		auto ResolveWritablePath(const UserModelStoreConfig &config, const std::string &user,
		                         bool create_directory) -> UserModelPathResult {
			if (!create_directory) {
				return ResolvePath(config, user);
			}

			const auto model_path = ResolveUserModelPath(config.models_dir, user);
			if (!model_path) {
				return UserModelPathResult{
				    .status        = UserModelStatus::kInvalidUser,
				    .error_message = kInvalidUserNameMessage,
				};
			}

			std::error_code ec;
			if (!std::filesystem::exists(config.models_dir, ec)) {
				if (ec) {
					return UserModelPathResult{
					    .status        = UserModelStatus::kParseError,
					    .error_message = WithErrno("Failed to inspect user models directory: " +
						                               config.models_dir.string(),
						                           ec.value()),
					};
				}
				const auto create_result = CreateSecureDirectoryTree(
				    config.models_dir, kUserModelsDirectoryLabel, kUserModelsDirMode, 0700,
				    config.owner_uid, config.validation_root);
				if (!create_result.ok) {
					return UserModelPathResult{
					    .status        = create_result.error_code == 0
						                     ? UserModelStatus::kInsecurePath
						                     : UserModelStatus::kDirectoryCreateFailed,
					    .error_message = create_result.error_code == 0
						                     ? create_result.error_message
						                     : "Failed to create secure user models directory: " +
						                           config.models_dir.string(),
					};
				}
			}

			// Then the same walk and classification readers use. A free model path is no failure
			// here; a models directory gone since the check above was removed under this writer.
			auto resolved = ResolvePath(config, user);
			if (resolved.status == UserModelStatus::kNoModel) {
				return UserModelPathResult{
				    .status = UserModelStatus::kOk,
				    .path   = std::move(resolved.path),
				};
			}
			if (resolved.status == UserModelStatus::kNoModelDirectory) {
				return UserModelPathResult{
				    .status        = UserModelStatus::kModelChanged,
				    .error_message = std::string(kUserModelChangedMessage),
				};
			}
			return resolved;
		}

		auto LockAndRevalidateForMutation(const UserModelStoreConfig &config,
		                                  const std::string &user, bool allow_missing)
		    -> LockedMutationTarget {
			const auto path_result = ResolveWritablePath(config, user, allow_missing);
			if (path_result.status != UserModelStatus::kOk) {
				return {
				    .failure = MapMutationStepFailure(path_result.status, path_result.error_message,
					                                  allow_missing),
				};
			}
			if (!allow_missing) {
				if (auto failure = VerifyExistingRegularFile(path_result.path)) {
					return {.failure = std::move(failure)};
				}
			}
			if (config.hooks.before_lock) {
				config.hooks.before_lock(path_result.path);
			}
			auto opened_model =
			    LockNamespaceAndOpenModelFile(path_result.path, config.owner_uid, allow_missing);
			if (opened_model.status != UserModelStatus::kOk) {
				return {
				    .failure = MapMutationStepFailure(
				        opened_model.status, std::move(opened_model.error_message), allow_missing),
				};
			}
			if (config.hooks.after_lock_before_revalidate) {
				config.hooks.after_lock_before_revalidate(path_result.path);
			}

			// Never create the models directory after the lock: one missing now was removed under
			// this writer, so its lock no longer serializes anything.
			const auto secured_path = ResolvePath(config, user);
			if (secured_path.status == UserModelStatus::kNoModelDirectory) {
				return {.failure = ModelChangedFailure()};
			}
			if (secured_path.status != UserModelStatus::kOk &&
			    (!allow_missing || secured_path.status != UserModelStatus::kNoModel)) {
				return {
				    .failure = MapMutationStepFailure(secured_path.status,
					                                  secured_path.error_message, allow_missing),
				};
			}
			const auto lock_still_held =
			    LockPathStillNamesLock(path_result.path, opened_model.namespace_lock.Get());
			if (!lock_still_held.has_value()) {
				const int error_number = errno;
				return {
				    .failure = MutationFailure(
				        UserModelStatus::kParseError,
				        UserModelInspectionFailedMessage(ModelNamespaceLockPath(path_result.path),
						                                 error_number)),
				};
			}
			if (!*lock_still_held) {
				return {.failure = ModelChangedFailure()};
			}
			if (!allow_missing) {
				if (auto failure = VerifyExistingRegularFile(path_result.path)) {
					return {.failure = std::move(failure)};
				}
			}
			if (auto recheck = RecheckOpenedPath(path_result.path, opened_model.file.Get());
			    recheck.status != UserModelStatus::kOk) {
				return {
				    .failure = MutationFailure(recheck.status, std::move(recheck.error_message)),
				};
			}
			return {
			    .path   = path_result.path,
			    .opened = std::move(opened_model),
			};
		}

		auto PopulateMutationContext(const UserModelStoreConfig  &config,
		                             const std::filesystem::path &path, int opened_fd,
		                             const MutationOptions &options, MutationContext *context)
		    -> std::optional<UserModelMutationResult> {
			const bool allow_missing = options.existence == MutationExistence::kAllowMissing;
			if (options.load_document) {
				context->document =
				    opened_fd >= 0
				        ? LoadDocumentFromFd(opened_fd, path,
				                             {.backend = {}, .model = {}, .strict_shape = true},
				                             config.owner_uid)
				        : user_model_codec::Document(
				              UserModelListResult{.status = UserModelStatus::kNoModel});
				const auto &entries = context->document.Result();
				if (entries.status != UserModelStatus::kOk &&
				    (!allow_missing || entries.status != UserModelStatus::kNoModel)) {
					return MutationFailure(entries.status, entries.error_message);
				}
				return std::nullopt;
			}

			// No model was open (a missing model a mutation allows): there is nothing to snapshot.
			if (opened_fd < 0) {
				return std::nullopt;
			}
			struct stat opened_stat{};
			if (fstat(opened_fd, &opened_stat) != 0) {
				const int error_number = errno;
				return MutationFailure(UserModelStatus::kParseError,
				                       UserModelInspectionFailedMessage(path, error_number));
			}
			context->snapshot = SnapshotFromStat(opened_stat);
			return std::nullopt;
		}

		auto CommitMutationDecision(const UserModelStoreTransaction  &transaction,
		                            const user_model_codec::Document &document,
		                            MutationDecision decision) -> UserModelMutationResult {
			switch (decision.action) {
				case MutationAction::kAbort:
					// An abort never reports success, whatever status the decider gave it.
					if (decision.status == UserModelStatus::kOk) {
						return MutationFailure(UserModelStatus::kParseError,
						                       "Internal user model store invariant failed");
					}
					return MutationFailure(decision.status, std::move(decision.error_message));
				case MutationAction::kWrite: {
					const auto commit_result = transaction.WriteDocument(document);
					if (!AtomicFileCommitIsDurable(commit_result)) {
						return CommitFailure(commit_result, UserModelStatus::kWriteFailed,
						                     std::move(decision.failure_message),
						                     std::move(decision.sync_failed_message),
						                     std::move(decision.entry));
					}
					return UserModelMutationResult{
					    .status = UserModelStatus::kOk,
					    .entry  = std::move(decision.entry),
					};
				}
				case MutationAction::kRemove: {
					const auto commit_result = transaction.RemoveFile();
					if (!AtomicFileCommitIsDurable(commit_result)) {
						return CommitFailure(commit_result, UserModelStatus::kDeleteFailed,
						                     std::move(decision.failure_message),
						                     std::move(decision.sync_failed_message),
						                     std::move(decision.entry), true);
					}
					return UserModelMutationResult{
					    .status       = UserModelStatus::kOk,
					    .entry        = std::move(decision.entry),
					    .removed_last = true,
					};
				}
			}
			return MutationFailure(UserModelStatus::kParseError,
			                       "Internal user model store invariant failed");
		}

	}  // namespace

	auto Mutate(const UserModelStoreConfig &config, const std::string &user,
	            const MutationOptions &options, const MutationDecider &decide)
	    -> UserModelMutationResult {
		const bool allow_missing = options.existence == MutationExistence::kAllowMissing;
		auto       locked        = LockAndRevalidateForMutation(config, user, allow_missing);
		if (locked.failure.has_value()) {
			return std::move(*locked.failure);
		}

		MutationContext context;
		if (auto failure = PopulateMutationContext(config, locked.path, locked.opened.file.Get(),
		                                           options, &context)) {
			return std::move(*failure);
		}

		UserModelStoreTransaction const transaction(std::move(locked.path),
		                                            std::move(locked.opened.namespace_lock),
		                                            std::move(locked.opened.file), config.hooks);
		return CommitMutationDecision(transaction, context.document, decide(context));
	}

}  // namespace howdy::native::user_model_store_internal
