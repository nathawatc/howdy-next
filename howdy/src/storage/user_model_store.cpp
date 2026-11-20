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
#include <sys/syscall.h>
#include <sys/types.h>

#include <linux/fs.h>

namespace howdy::native {

	namespace {

		constexpr mode_t kUserModelsDirMode = S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IXGRP;
		constexpr mode_t kUserModelFileMode = S_IRUSR | S_IWUSR;
		// Root-only: a user who can open the lock file can hold it and stall every writer.
		constexpr mode_t           kUserModelLockFileMode = S_IRUSR | S_IWUSR;
		constexpr std::string_view kUserModelTempPrefix   = ".howdy-user-model-";
		constexpr auto             kUserModelFileTooLargeMessage =
		    "User model file is too large or unreadable: ";
		constexpr auto kUserModelFileReadFailedMessage = "Failed to read user model file: ";
		constexpr auto kModelFileLockFailedMessage     = "Failed to lock or open model file";

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
			auto const &hooks = user_model_store_test_hooks::Current();
			const auto  injected_errno =
			    is_rollback ? hooks.rollback_exchange_errno : hooks.exchange_errno;
			if (injected_errno.has_value()) {
				errno = *injected_errno;
				return ExchangeResultForErrno(errno);
			}

#if defined(SYS_renameat2) && defined(RENAME_EXCHANGE)
			if (RetryOnEintr([&] -> decltype(syscall(0)) {
				    return syscall(SYS_renameat2, AT_FDCWD, left.c_str(), AT_FDCWD, right.c_str(),
					               RENAME_EXCHANGE);
			    }) != 0) {
				return ExchangeResultForErrno(errno);
			}
			return ExchangeResult::kSuccess;
#else
			(void)left;
			(void)right;
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
		                           const user_model_codec::Document &document, int opened_fd)
		    -> AtomicFileCommitResult {
			const auto serialized = user_model_codec::SerializeDocument(document);
			if (!serialized.has_value() ||
			    serialized->size() > user_model_limits::kMaxUserModelFileBytes ||
			    !CleanupStaleWriteArtifacts(path)) {
				return AtomicFileCommitResult::kNotCommitted;
			}
			auto staged = PrepareStagedFile(path, kUserModelTempPrefix, kUserModelFileMode,
			                                StagedFileMetadataPolicy::kPreserveExisting,
			                                StagedFileParentPolicy::kRequireExisting);
			if (!staged.has_value()) {
				return AtomicFileCommitResult::kNotCommitted;
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
			if (!PathIdentifiesFd(path, opened_fd)) {
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
			if (!PathIdentifiesFd(staged->path, opened_fd)) {
				if (ExchangePaths(staged->path, path, true) == ExchangeResult::kSuccess) {
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

		auto RemoveOpenedFile(const std::filesystem::path &path, int fd) -> AtomicFileCommitResult {
			if (!PathIdentifiesFd(path, fd)) {
				return AtomicFileCommitResult::kNotCommitted;
			}
			auto const &hooks = user_model_store_test_hooks::Current();
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

		struct OpenedUserModelFile {
			ScopedFileLock namespace_lock;
			ScopedFd       file;
			bool           created_empty_file = false;
		};

		// `<models_dir>.lock`, beside the models directory rather than inside it, so nothing
		// that lists the directory meets it. The trailing separator of a directory named
		// `.../models/` is dropped first; it would otherwise put the lock inside.
		auto ModelNamespaceLockPath(const std::filesystem::path &model_path)
		    -> std::filesystem::path {
			auto models_dir = model_path.parent_path().lexically_normal();
			if (!models_dir.has_filename()) {
				models_dir = models_dir.parent_path();
			}
			return LockFilePath(models_dir);
		}

		auto LockModelNamespace(const std::filesystem::path &model_path)
		    -> std::optional<ScopedFileLock> {
			// Serializes cooperating UserModelStore writers only. Parent-directory write
			// permission still allows uncooperative actors to rename or unlink entries.
			// Writers lock a root-only file, never the models directory or a model file: flock
			// needs only a readable descriptor, and users can open a 0755 models directory and
			// get a read-only descriptor to their own model from the auth helper, so a lock on
			// either would let a user stall every writer. The lock file's parent is the models
			// directory's parent, which Resolve checked with the rest of the models directory
			// tree before either caller gets here.
			const auto lock_path = ModelNamespaceLockPath(model_path);
			ScopedFd   fd(open(lock_path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC,
			                   kUserModelLockFileMode));
			if (!fd.Valid()) {
				return std::nullopt;
			}
			struct stat lock_stat{};
			if (fstat(fd.Get(), &lock_stat) != 0 ||
			    !CheckSecurePathStat(lock_stat, SecurePathKind::kRegularFile, lock_path,
			                         kUserModelLockFileLabel, DefaultSecureOwnerUid())
			         .ok ||
			    HasAnyBits(lock_stat.st_mode, S_IRWXG, S_IRWXO)) {
				return std::nullopt;
			}
			if (RetryOnEintr([&] -> int {
				    return flock(fd.Get(), LOCK_EX);
			    }) != 0) {
				return std::nullopt;
			}

			return ScopedFileLock(std::move(fd));
		}

		auto LockNamespaceAndOpenModelFile(const std::filesystem::path &path,
		                                   bool                         create_if_missing)
		    -> std::optional<OpenedUserModelFile> {
			auto namespace_lock = LockModelNamespace(path);
			if (!namespace_lock.has_value()) {
				return std::nullopt;
			}
			ScopedFd fd(open(path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW));
			bool     created_empty_file = false;
			if (!fd.Valid() && errno == ENOENT && create_if_missing) {
				fd.Reset(open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
				              kUserModelFileMode));
				created_empty_file = fd.Valid();
			}
			if (!fd.Valid()) {
				return std::nullopt;
			}
			struct stat opened_file{};
			if (fstat(fd.Get(), &opened_file) != 0 ||
			    !OpenedModelFileIsSecure(opened_file, DefaultSecureOwnerUid())) {
				return std::nullopt;
			}

			return OpenedUserModelFile{
			    .namespace_lock     = std::move(*namespace_lock),
			    .file               = std::move(fd),
			    .created_empty_file = created_empty_file,
			};
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
	                                                     ScopedFd file, bool created_empty_file)
	    : path_(std::move(path))
	    , namespace_lock_(std::move(namespace_lock))
	    , file_(std::move(file))
	    , created_empty_file_(created_empty_file) {}

	UserModelStoreTransaction::~UserModelStoreTransaction() {
		if (!created_empty_file_ || completed_ || !PathMatchesOpenedFile()) {
			return;
		}
		struct stat opened_file{};
		if (fstat(file_.Get(), &opened_file) != 0 || opened_file.st_size != 0) {
			return;
		}
		(void)RemoveOpenedFile(path_, file_.Get());
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

	auto UserModelStoreTransaction::PathMatchesOpenedFile() const -> bool {
		return PathIdentifiesFd(path_, file_.Get());
	}

	auto UserModelStoreTransaction::WriteDocument(const user_model_codec::Document &document) const
	    -> AtomicFileCommitResult {
		if (!PathMatchesOpenedFile()) {
			return AtomicFileCommitResult::kNotCommitted;
		}
		const auto result = WriteModelsAtomically(path_, document, file_.Get());
		if (AtomicFileMayHaveCommitted(result)) {
			completed_ = true;
		}
		return result;
	}

	auto UserModelStoreTransaction::RemoveFile() const -> AtomicFileCommitResult {
		const auto result = RemoveOpenedFile(path_, file_.Get());
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
	                                        const OpenedModelReadPolicy &policy)
	    -> user_model_codec::Document {
		struct stat opened_file{};
		if (fd < 0 || fstat(fd, &opened_file) != 0) {
			return user_model_codec::Document{
			    StoreListFailure(UserModelStatus::kParseError,
				                 "Failed to inspect opened user model file: " + path.string()),
			};
		}
		if (!OpenedModelFileIsSecure(opened_file, policy.owner_uid)) {
			return user_model_codec::Document{
			    StoreListFailure(UserModelStatus::kInsecurePath,
				                 "Opened user model file failed security validation: " +
				                     path.string()),
			};
		}
		if (opened_file.st_size == 0 && policy.treat_empty_as_no_model) {
			return user_model_codec::Document(
			    UserModelListResult{.status = UserModelStatus::kNoModel});
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
			if (errno == ENOENT) {
				return user_model_codec::Document(
				    UserModelListResult{.status = UserModelStatus::kNoModel});
			}
			return user_model_codec::Document{
			    StoreListFailure(UserModelStatus::kParseError,
				                 "Failed to open user model file: " + path.string()),
			};
		}
		return LoadDocumentFromFd(input.Get(), path, expectations,
		                          {
		                              .owner_uid               = DefaultSecureOwnerUid(),
		                              .treat_empty_as_no_model = false,
		                          });
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
		auto opened_model = LockNamespaceAndOpenModelFile(path_result.path, true);
		if (!opened_model.has_value()) {
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
		if (!PathIdentifiesFd(path_result.path, opened_model->file.Get())) {
			return UserModelStoreMutationResult{
			    .document = user_model_codec::Document(StoreListFailure(
			        UserModelStatus::kModelChanged, std::string(kUserModelChangedMessage))),
			};
		}
		auto document =
		    LoadDocumentFromFd(opened_model->file.Get(), path_result.path,
			                   {.backend = {}, .model = {}, .strict_shape = true},
			                   {
		                           .owner_uid               = DefaultSecureOwnerUid(),
		                           .treat_empty_as_no_model = opened_model->created_empty_file,
		                       });
		return UserModelStoreMutationResult{
		    .transaction = UserModelStoreTransaction(
		        path_result.path, std::move(opened_model->namespace_lock),
		        std::move(opened_model->file), opened_model->created_empty_file),
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
		if (!opened_model.has_value()) {
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
		if (!PathIdentifiesFd(path_result.path, opened_model->file.Get())) {
			return UserModelStoreTransactionResult{
			    .status        = UserModelStatus::kModelChanged,
			    .error_message = std::string(kUserModelChangedMessage),
			};
		}
		return UserModelStoreTransactionResult{
		    .status      = UserModelStatus::kOk,
		    .transaction = UserModelStoreTransaction(
		        path_result.path, std::move(opened_model->namespace_lock),
		        std::move(opened_model->file), opened_model->created_empty_file),
		};
	}

}  // namespace howdy::native
