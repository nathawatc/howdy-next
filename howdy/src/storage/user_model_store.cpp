#include "storage/user_model_store.hpp"

#include "config/runtime_paths.hpp"
#include "storage/user_model_limits.hpp"
#include "storage/user_model_readiness.hpp"
#include "storage/user_model_status.hpp"
#include "storage/user_model_store/test_hooks.hpp"
#include "support/atomic_files.hpp"
#include "support/fd_io.hpp"
#include "support/file_security.hpp"
#include "support/user_names.hpp"

#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

#include <sys/stat.h>
#include <sys/syscall.h>

#include <linux/fs.h>

namespace howdy::native {

	namespace {

		constexpr mode_t kUserModelsDirMode = S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IXGRP;
		constexpr mode_t kUserModelFileMode = S_IRUSR | S_IWUSR;
		constexpr std::string_view kUserModelTempPrefix = ".howdy-user-model-";
		constexpr auto             kUserModelFileTooLargeMessage =
		    "User model file is too large or unreadable: ";
		constexpr auto kUserModelFileReadFailedMessage = "Failed to read user model file: ";
		constexpr auto kModelFileLockFailedMessage     = "Failed to lock model file";

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

		auto OpenedModelFileIsSecure(const struct stat &opened_file) -> bool {
			const auto owner_uid = DefaultSecureOwnerUid();
			return S_ISREG(opened_file.st_mode) &&
			       (!owner_uid.has_value() || opened_file.st_uid == *owner_uid) &&
			       (opened_file.st_mode & (S_IWGRP | S_IWOTH)) == 0 && opened_file.st_nlink == 1;
		}

		auto OpenedModelFileIsSecureForRead(int fd, const std::filesystem::path &path,
		                                    const struct stat &opened_file) -> bool {
			return OpenedModelFileIsSecure(opened_file) ||
			       (opened_file.st_nlink == 2 && ValidateStagedUserModelFile(fd, path));
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
			    .mtime_seconds  = static_cast<long long>(st.st_mtim.tv_sec),
			    .mtime_nanosecs = static_cast<long long>(st.st_mtim.tv_nsec),
			    .ctime_seconds  = static_cast<long long>(st.st_ctim.tv_sec),
			    .ctime_nanosecs = static_cast<long long>(st.st_ctim.tv_nsec),
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

		auto PathIdentifiesFd(const std::filesystem::path &path, int fd) -> bool {
			struct stat fd_stat{};
			struct stat path_stat{};
			return fd >= 0 && fstat(fd, &fd_stat) == 0 && lstat(path.c_str(), &path_stat) == 0 &&
			       SameFileIdentity(fd_stat, path_stat);
		}

		enum class ExchangeResult : std::uint8_t {
			kSuccess,
			kFailed,
			kUnsupported,
		};

		auto ExchangeResultForErrno(int error_number) -> ExchangeResult {
			if (error_number == ENOSYS || error_number == EINVAL || error_number == EOPNOTSUPP) {
				return ExchangeResult::kUnsupported;
			}
#ifdef ENOTSUP
			if (error_number == ENOTSUP) {
				return ExchangeResult::kUnsupported;
			}
#endif
			return ExchangeResult::kFailed;
		}

		auto ExchangePaths(const std::filesystem::path &left, const std::filesystem::path &right,
		                   bool is_rollback) -> ExchangeResult {
			auto      &hooks = user_model_store_test_hooks::Current();
			const auto injected_errno =
			    is_rollback ? hooks.rollback_exchange_errno : hooks.exchange_errno;
			if (injected_errno.has_value()) {
				errno = *injected_errno;
				return ExchangeResultForErrno(errno);
			}

#if defined(SYS_renameat2) && defined(RENAME_EXCHANGE)
			while (syscall(SYS_renameat2, AT_FDCWD, left.c_str(), AT_FDCWD, right.c_str(),
			               RENAME_EXCHANGE) != 0) {
				if (errno != EINTR) {
					return ExchangeResultForErrno(errno);
				}
			}
			return ExchangeResult::kSuccess;
#else
			errno = ENOSYS;
			return ExchangeResult::kUnsupported;
#endif
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

		auto WriteModelsAtomically(const std::filesystem::path      &path,
		                           const user_model_codec::Document &document, int locked_fd)
		    -> AtomicFileCommitResult {
			const auto serialized = user_model_codec::SerializeDocument(document);
			if (!serialized.has_value() ||
			    serialized->size() > user_model_limits::kMaxUserModelFileBytes ||
			    !CleanupStaleWriteArtifacts(path)) {
				return AtomicFileCommitResult::kNotCommitted;
			}
			auto staged = PrepareStagedFile(path, kUserModelTempPrefix, kUserModelFileMode);
			if (!staged.has_value()) {
				return AtomicFileCommitResult::kNotCommitted;
			}
			auto &hooks = user_model_store_test_hooks::Current();
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
			if (!PathIdentifiesFd(path, locked_fd)) {
				CleanupStagedFile(*staged);
				return AtomicFileCommitResult::kNotCommitted;
			}
			if (hooks.after_write_identity_check) {
				hooks.after_write_identity_check(path);
			}
			if (!staged->fd.Close()) {
				CleanupStagedFile(*staged);
				return AtomicFileCommitResult::kNotCommitted;
			}
			const auto exchange_result = ExchangePaths(staged->path, path, false);
			if (exchange_result != ExchangeResult::kSuccess) {
				CleanupStagedFile(*staged);
				return exchange_result == ExchangeResult::kUnsupported
				           ? AtomicFileCommitResult::kAtomicExchangeUnsupported
				           : AtomicFileCommitResult::kNotCommitted;
			}
			if (!PathIdentifiesFd(staged->path, locked_fd)) {
				if (!hooks.fail_write_rollback &&
				    ExchangePaths(staged->path, path, true) == ExchangeResult::kSuccess) {
					CleanupStagedFile(*staged);
					return AtomicFileCommitResult::kNotCommitted;
				}
				staged->path.clear();
				return AtomicFileCommitResult::kStateUncertain;
			}

			// Exchange commits the canonical write; old-file removal is cleanup only.
			std::error_code ec;
			if (!hooks.fail_write_cleanup) {
				std::filesystem::remove(staged->path, ec);
			}
			const bool parent_synced = !hooks.fail_parent_sync && SyncParentDirectory(path);
			staged->path.clear();
			return parent_synced ? AtomicFileCommitResult::kCommitted
			                     : AtomicFileCommitResult::kCommittedSyncFailed;
		}

		auto RemoveLockedFile(const std::filesystem::path &path, int fd) -> AtomicFileCommitResult {
			if (!PathIdentifiesFd(path, fd)) {
				return AtomicFileCommitResult::kNotCommitted;
			}
			auto &hooks = user_model_store_test_hooks::Current();
			if (hooks.before_delete_commit) {
				hooks.before_delete_commit(path);
			}

			std::error_code ec;
			if (hooks.fail_delete_unlink) {
				ec = std::make_error_code(std::errc::permission_denied);
			} else {
				std::filesystem::remove(path, ec);
			}
			if (ec) {
				return AtomicFileCommitResult::kNotCommitted;
			}
			return !hooks.fail_parent_sync && SyncParentDirectory(path)
			           ? AtomicFileCommitResult::kCommitted
			           : AtomicFileCommitResult::kCommittedSyncFailed;
		}

		struct LockedUserModelFile {
			ScopedFileLock namespace_lock;
			ScopedFileLock lock;
			bool           created_empty_file = false;
		};

		auto LockModelNamespace(const std::filesystem::path &path)
		    -> std::optional<ScopedFileLock> {
			// Serializes cooperating UserModelStore writers only. Parent-directory write
			// permission still allows uncooperative actors to rename or unlink entries.
			const int fd =
			    open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
			if (fd < 0) {
				return std::nullopt;
			}
			while (flock(fd, LOCK_EX) != 0) {
				if (errno != EINTR) {
					close(fd);
					return std::nullopt;
				}
			}

			ScopedFileLock lock;
			lock.fd   = fd;
			lock.path = path.parent_path();
			return lock;
		}

		auto OpenAndLockModelFile(const std::filesystem::path &path, bool create_if_missing)
		    -> std::optional<LockedUserModelFile> {
			auto namespace_lock = LockModelNamespace(path);
			if (!namespace_lock.has_value()) {
				return std::nullopt;
			}
			int  fd                 = open(path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
			bool created_empty_file = false;
			if (fd < 0 && errno == ENOENT && create_if_missing) {
				fd = open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
				          kUserModelFileMode);
				created_empty_file = fd >= 0;
			}
			if (fd < 0) {
				return std::nullopt;
			}
			struct stat opened_file{};
			if (fstat(fd, &opened_file) != 0 || !OpenedModelFileIsSecure(opened_file)) {
				close(fd);
				return std::nullopt;
			}
			while (flock(fd, LOCK_EX) != 0) {
				if (errno != EINTR) {
					close(fd);
					return std::nullopt;
				}
			}

			ScopedFileLock lock;
			lock.fd   = fd;
			lock.path = path;
			return LockedUserModelFile{.namespace_lock     = std::move(*namespace_lock),
			                           .lock               = std::move(lock),
			                           .created_empty_file = created_empty_file};
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

	UserModelStoreTransaction::UserModelStoreTransaction(std::filesystem::path path,
	                                                     ScopedFileLock        namespace_lock,
	                                                     ScopedFileLock        lock,
	                                                     bool                  created_empty_file)
	    : path_(std::move(path))
	    , namespace_lock_(std::move(namespace_lock))
	    , lock_(std::move(lock))
	    , created_empty_file_(created_empty_file) {}

	UserModelStoreTransaction::~UserModelStoreTransaction() {
		if (!created_empty_file_ || completed_ || !PathMatchesLockedFile()) {
			return;
		}
		struct stat opened_file{};
		if (fstat(lock_.fd, &opened_file) != 0 || opened_file.st_size != 0) {
			return;
		}
		(void)RemoveLockedFile(path_, lock_.fd);
	}

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

	auto UserModelStoreTransaction::PathMatchesLockedFile() const -> bool {
		return PathIdentifiesFd(path_, lock_.fd);
	}

	auto UserModelStoreTransaction::WriteDocument(const user_model_codec::Document &document) const
	    -> AtomicFileCommitResult {
		if (!PathMatchesLockedFile()) {
			return AtomicFileCommitResult::kNotCommitted;
		}
		const auto result = WriteModelsAtomically(path_, document, lock_.fd);
		if (AtomicFileMayHaveCommitted(result)) {
			completed_ = true;
		}
		return result;
	}

	auto UserModelStoreTransaction::RemoveFile() const -> AtomicFileCommitResult {
		const auto result = RemoveLockedFile(path_, lock_.fd);
		if (AtomicFileMayHaveCommitted(result)) {
			completed_ = true;
		}
		return result;
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
				        "Failed to inspect user models directory: " + models_dir.string(),
				};
			}
			std::filesystem::create_directories(models_dir, ec);
			if (ec || chmod(models_dir.c_str(), kUserModelsDirMode) != 0) {
				return UserModelPathResult{
				    .status = UserModelStatus::kDirectoryCreateFailed,
				    .error_message =
				        "Failed to create secure user models directory: " + models_dir.string(),
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
			    .error_message = std::string(kUserModelFileInspectionFailedMessage) + ": " +
			                     model_path->string(),
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
			*message = std::string(kUserModelFileInspectionFailedMessage) + ": " + path.string();
			return UserModelStatus::kParseError;
		}
		return UserModelStatus::kNoModel;
	}

	auto UserModelStore::LoadDocumentFromFd(int fd, const std::filesystem::path &path,
	                                        const UserModelExpectations &expectations,
	                                        bool                         treat_empty_as_no_model)
	    -> user_model_codec::Document {
		struct stat opened_file{};
		if (fd < 0 || fstat(fd, &opened_file) != 0) {
			return user_model_codec::Document{
			    StoreListFailure(UserModelStatus::kParseError,
			                     "Failed to inspect opened user model file: " + path.string()),
			};
		}
		if (!OpenedModelFileIsSecureForRead(fd, path, opened_file)) {
			return user_model_codec::Document{
			    StoreListFailure(UserModelStatus::kInsecurePath,
			                     "Opened user model file failed security validation: " +
			                         path.string()),
			};
		}
		if (opened_file.st_size == 0 && treat_empty_as_no_model) {
			return user_model_codec::Document(
			    UserModelListResult{.status = UserModelStatus::kNoModel});
		}
		if (opened_file.st_size < 0 ||
		    std::cmp_greater(opened_file.st_size, user_model_limits::kMaxUserModelFileBytes)) {
			return user_model_codec::Document(
			    StoreListFailure(UserModelStatus::kOversized,
			                     std::string(kUserModelFileTooLargeMessage) + path.string()));
		}
		if (lseek(fd, 0, SEEK_SET) < 0) {
			return user_model_codec::Document{
			    StoreListFailure(UserModelStatus::kParseError,
			                     std::string(kUserModelFileReadFailedMessage) + path.string()),
			};
		}

		const auto content = ReadFdToStringBounded(
		    {.fd = fd, .max_bytes = user_model_limits::kMaxUserModelFileBytes + 1});
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
		                                        expectations.metric, expectations.model,
		                                        expectations.strict_shape);
	}

	auto UserModelStore::LoadDocumentFromPath(const std::filesystem::path &path,
	                                          const UserModelExpectations &expectations)
	    -> user_model_codec::Document {
		ScopedFd input(open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
		if (input.Get() < 0) {
			if (errno == ENOENT) {
				return user_model_codec::Document(
				    UserModelListResult{.status = UserModelStatus::kNoModel});
			}
			return user_model_codec::Document{
			    StoreListFailure(UserModelStatus::kParseError,
			                     "Failed to open user model file: " + path.string()),
			};
		}
		return LoadDocumentFromFd(input.Get(), path, expectations, false);
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
			return InspectFailure(UserModelStatus::kParseError,
			                      std::string(kUserModelFileInspectionFailedMessage) + ": " +
			                          path_result.path.string());
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
		auto locked_file = OpenAndLockModelFile(path_result.path, true);
		if (!locked_file.has_value()) {
			return UserModelStoreMutationResult{
			    .document = user_model_codec::Document(
			        StoreListFailure(UserModelStatus::kLockFailed, kModelFileLockFailedMessage)),
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
		if (!PathIdentifiesFd(path_result.path, locked_file->lock.fd)) {
			return UserModelStoreMutationResult{
			    .document = user_model_codec::Document(StoreListFailure(
			        UserModelStatus::kModelChanged, std::string(kUserModelChangedMessage))),
			};
		}
		auto document =
		    LoadDocumentFromFd(locked_file->lock.fd, path_result.path,
		                       {.backend = {}, .metric = {}, .model = {}, .strict_shape = true},
		                       locked_file->created_empty_file);
		return UserModelStoreMutationResult{
		    .transaction = UserModelStoreTransaction(
		        path_result.path, std::move(locked_file->namespace_lock),
		        std::move(locked_file->lock), locked_file->created_empty_file),
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
		auto locked_file = OpenAndLockModelFile(path_result.path, false);
		if (!locked_file.has_value()) {
			return UserModelStoreTransactionResult{
			    .status        = UserModelStatus::kLockFailed,
			    .error_message = kModelFileLockFailedMessage,
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
		if (!PathIdentifiesFd(path_result.path, locked_file->lock.fd)) {
			return UserModelStoreTransactionResult{
			    .status        = UserModelStatus::kModelChanged,
			    .error_message = std::string(kUserModelChangedMessage),
			};
		}
		return UserModelStoreTransactionResult{
		    .status      = UserModelStatus::kOk,
		    .transaction = UserModelStoreTransaction(
		        path_result.path, std::move(locked_file->namespace_lock),
		        std::move(locked_file->lock), locked_file->created_empty_file),
		};
	}

}  // namespace howdy::native
