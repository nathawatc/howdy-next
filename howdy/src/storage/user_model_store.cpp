#include "storage/user_model_store.hpp"

#include "config/runtime_paths.hpp"
#include "storage/user_model_limits.hpp"
#include "storage/user_model_readiness.hpp"
#include "storage/user_model_store_test_hooks.hpp"
#include "support/atomic_files.hpp"
#include "support/fd_io.hpp"
#include "support/file_security.hpp"
#include "support/user_names.hpp"

#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <utility>

#include <sys/stat.h>
#include <sys/syscall.h>

#include <linux/fs.h>

namespace howdy::native {

	namespace {

		constexpr mode_t kUserModelsDirMode = S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IXGRP;
		constexpr mode_t kUserModelFileMode = S_IRUSR | S_IWUSR;
		constexpr std::string_view kUserModelTempPrefix = ".howdy-user-model-";

		auto failure(UserModelStatus status, std::string message) -> UserModelListResult {
			return UserModelListResult{
			    .status        = status,
			    .error_message = std::move(message),
			};
		}

		auto inspect_failure(UserModelStatus status, std::string message)
		    -> UserModelInspectResult {
			return UserModelInspectResult{
			    .status        = status,
			    .error_message = std::move(message),
			};
		}

		auto opened_model_file_is_secure(const struct stat &opened_file) -> bool {
			const auto owner_uid = default_secure_owner_uid();
			return S_ISREG(opened_file.st_mode) &&
			       (!owner_uid.has_value() || opened_file.st_uid == *owner_uid) &&
			       (opened_file.st_mode & (S_IWGRP | S_IWOTH)) == 0 && opened_file.st_nlink == 1;
		}

		auto snapshot_model_file(const std::filesystem::path &path)
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

		auto snapshots_match(const UserModelFileSnapshot &left, const UserModelFileSnapshot &right)
		    -> bool {
			return left.dev == right.dev && left.inode == right.inode && left.size == right.size &&
			       left.mtime_seconds == right.mtime_seconds &&
			       left.mtime_nanosecs == right.mtime_nanosecs &&
			       left.ctime_seconds == right.ctime_seconds &&
			       left.ctime_nanosecs == right.ctime_nanosecs;
		}

		auto same_file_identity(const struct stat &left, const struct stat &right) -> bool {
			return left.st_dev == right.st_dev && left.st_ino == right.st_ino;
		}

		auto path_identifies_fd(const std::filesystem::path &path, int fd) -> bool {
			struct stat fd_stat{};
			struct stat path_stat{};
			return fd >= 0 && fstat(fd, &fd_stat) == 0 && lstat(path.c_str(), &path_stat) == 0 &&
			       same_file_identity(fd_stat, path_stat);
		}

		auto exchange_paths(const std::filesystem::path &left, const std::filesystem::path &right)
		    -> bool {
#ifdef SYS_renameat2
			while (syscall(SYS_renameat2, AT_FDCWD, left.c_str(), AT_FDCWD, right.c_str(),
			               RENAME_EXCHANGE) != 0) {
				if (errno != EINTR) {
					return false;
				}
			}
			return true;
#else
			errno = ENOSYS;
			return false;
#endif
		}

		auto cleanup_stale_write_artifacts(const std::filesystem::path &path) -> bool {
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

		auto write_models_atomically(const std::filesystem::path      &path,
		                             const user_model_codec::Document &document, int locked_fd)
		    -> AtomicFileCommitResult {
			const auto serialized = user_model_codec::serialize_document(document);
			if (!serialized.has_value() ||
			    serialized->size() > user_model_limits::kMaxUserModelFileBytes ||
			    !cleanup_stale_write_artifacts(path)) {
				return AtomicFileCommitResult::kNotCommitted;
			}
			auto staged = prepare_staged_file(path, kUserModelTempPrefix, kUserModelFileMode);
			if (!staged.has_value()) {
				return AtomicFileCommitResult::kNotCommitted;
			}
			auto &hooks = user_model_store_test_hooks::current();
			if (hooks.fail_write || !write_all_to_fd(staged->fd.get(), *serialized)) {
				cleanup_staged_file(*staged);
				return AtomicFileCommitResult::kNotCommitted;
			}
			if (hooks.fail_fsync || !sync_fd(staged->fd.get())) {
				cleanup_staged_file(*staged);
				return AtomicFileCommitResult::kNotCommitted;
			}
			if (hooks.before_write_commit) {
				hooks.before_write_commit(path);
			}
			if (!path_identifies_fd(path, locked_fd)) {
				cleanup_staged_file(*staged);
				return AtomicFileCommitResult::kNotCommitted;
			}
			if (hooks.after_write_identity_check) {
				hooks.after_write_identity_check(path);
			}
			if (!staged->fd.close() || !exchange_paths(staged->path, path)) {
				cleanup_staged_file(*staged);
				return AtomicFileCommitResult::kNotCommitted;
			}
			if (!path_identifies_fd(staged->path, locked_fd)) {
				if (!hooks.fail_write_rollback && exchange_paths(staged->path, path)) {
					cleanup_staged_file(*staged);
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
			const bool parent_synced = !hooks.fail_parent_sync && sync_parent_directory(path);
			staged->path.clear();
			return parent_synced ? AtomicFileCommitResult::kCommitted
			                     : AtomicFileCommitResult::kCommittedSyncFailed;
		}

		auto remove_locked_file(const std::filesystem::path &path, int fd)
		    -> AtomicFileCommitResult {
			if (!path_identifies_fd(path, fd)) {
				return AtomicFileCommitResult::kNotCommitted;
			}
			auto &hooks = user_model_store_test_hooks::current();
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
			return !hooks.fail_parent_sync && sync_parent_directory(path)
			           ? AtomicFileCommitResult::kCommitted
			           : AtomicFileCommitResult::kCommittedSyncFailed;
		}

		struct LockedUserModelFile {
			ScopedFileLock namespace_lock;
			ScopedFileLock lock;
			bool           created_empty_file = false;
		};

		auto lock_model_namespace(const std::filesystem::path &path)
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

		auto open_and_lock_model_file(const std::filesystem::path &path, bool create_if_missing)
		    -> std::optional<LockedUserModelFile> {
			auto namespace_lock = lock_model_namespace(path);
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
			if (fstat(fd, &opened_file) != 0 || !opened_model_file_is_secure(opened_file)) {
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

		auto current() -> Hooks & {
			static Hooks hooks;
			return hooks;
		}

		ScopedHooks::ScopedHooks(Hooks hooks)
		    : previous_(std::move(current())) {
			current() = std::move(hooks);
		}

		ScopedHooks::~ScopedHooks() {
			current() = std::move(previous_);
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
		if (!created_empty_file_ || completed_ || !path_matches_locked_file()) {
			return;
		}
		struct stat opened_file{};
		if (fstat(lock_.fd, &opened_file) != 0 || opened_file.st_size != 0) {
			return;
		}
		(void)remove_locked_file(path_, lock_.fd);
	}

	auto UserModelStoreTransaction::path() const -> const std::filesystem::path & {
		return path_;
	}

	auto UserModelStoreTransaction::snapshot() const -> std::optional<UserModelFileSnapshot> {
		return snapshot_model_file(path_);
	}

	auto UserModelStoreTransaction::snapshot_matches(const UserModelFileSnapshot &expected) const
	    -> std::optional<bool> {
		const auto current = snapshot();
		if (!current.has_value()) {
			return std::nullopt;
		}
		return snapshots_match(*current, expected);
	}

	auto UserModelStoreTransaction::path_matches_locked_file() const -> bool {
		return path_identifies_fd(path_, lock_.fd);
	}

	auto UserModelStoreTransaction::write_document(const user_model_codec::Document &document) const
	    -> AtomicFileCommitResult {
		if (!path_matches_locked_file()) {
			return AtomicFileCommitResult::kNotCommitted;
		}
		const auto result = write_models_atomically(path_, document, lock_.fd);
		if (atomic_file_may_have_committed(result)) {
			completed_ = true;
		}
		return result;
	}

	auto UserModelStoreTransaction::remove_file() const -> AtomicFileCommitResult {
		const auto result = remove_locked_file(path_, lock_.fd);
		if (atomic_file_may_have_committed(result)) {
			completed_ = true;
		}
		return result;
	}

	auto UserModelStore::resolve(const std::string &user, bool create_directory,
	                             std::optional<uid_t> owner_uid)
	    -> UserModelStore::UserModelPathResult {
		const auto models_dir = resolve_user_models_dir();
		if (!create_directory) {
			const auto readiness = check_user_model_readiness(models_dir, user, owner_uid);
			return UserModelPathResult{
			    .status        = readiness.status,
			    .error_message = readiness.error_message,
			    .path          = readiness.path,
			};
		}

		const auto model_path = resolve_user_model_path(models_dir, user);
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

		const auto directory_security =
		    check_secure_root_owned_directory_tree(models_dir, "User models directory", owner_uid);
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
			    .error_message = "Failed to inspect user model file: " + model_path->string(),
			};
		}
		if (model_exists) {
			const auto file_security = check_secure_root_owned_file_with_directory(
			    *model_path, {.directory = "User models directory", .file = "User model file"},
			    owner_uid);
			if (!file_security.ok) {
				return UserModelPathResult{
				    .status        = UserModelStatus::kInsecurePath,
				    .error_message = file_security.error_message,
				};
			}
		}

		return UserModelPathResult{.path = *model_path};
	}

	auto UserModelStore::inspect_regular_file_status(const std::filesystem::path &path,
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
			*message = "Failed to inspect user model file: " + path.string();
			return UserModelStatus::kParseError;
		}
		return UserModelStatus::kNoModel;
	}

	auto UserModelStore::load_document_from_fd(int fd, const std::filesystem::path &path,
	                                           const UserModelExpectations &expectations,
	                                           bool                         treat_empty_as_no_model)
	    -> user_model_codec::Document {
		struct stat opened_file{};
		if (fd < 0 || fstat(fd, &opened_file) != 0) {
			return user_model_codec::Document{
			    failure(UserModelStatus::kParseError,
			            "Failed to inspect opened user model file: " + path.string()),
			};
		}
		if (!opened_model_file_is_secure(opened_file)) {
			return user_model_codec::Document{
			    failure(UserModelStatus::kInsecurePath,
			            "Opened user model file failed security validation: " + path.string()),
			};
		}
		if (opened_file.st_size == 0 && treat_empty_as_no_model) {
			return user_model_codec::Document(
			    UserModelListResult{.status = UserModelStatus::kNoModel});
		}
		if (opened_file.st_size < 0 ||
		    std::cmp_greater(opened_file.st_size, user_model_limits::kMaxUserModelFileBytes)) {
			return user_model_codec::Document(
			    failure(UserModelStatus::kOversized,
			            "User model file is too large or unreadable: " + path.string()));
		}
		if (lseek(fd, 0, SEEK_SET) < 0) {
			return user_model_codec::Document{
			    failure(UserModelStatus::kParseError,
			            "Failed to read user model file: " + path.string()),
			};
		}

		const auto content = read_fd_to_string_bounded(
		    {.fd = fd, .max_bytes = user_model_limits::kMaxUserModelFileBytes + 1});
		if (content.read_error) {
			return user_model_codec::Document{
			    failure(UserModelStatus::kParseError,
			            "Failed to read user model file: " + path.string()),
			};
		}
		if (content.hit_limit) {
			return user_model_codec::Document{
			    failure(UserModelStatus::kOversized,
			            "User model file is too large or unreadable: " + path.string()),
			};
		}

		return user_model_codec::decode_document(content.output, expectations.backend,
		                                         expectations.metric, expectations.model,
		                                         expectations.strict_shape);
	}

	auto UserModelStore::load_document_from_path(const std::filesystem::path &path,
	                                             const UserModelExpectations &expectations)
	    -> user_model_codec::Document {
		ScopedFd input(open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
		if (input.get() < 0) {
			if (errno == ENOENT) {
				return user_model_codec::Document(
				    UserModelListResult{.status = UserModelStatus::kNoModel});
			}
			return user_model_codec::Document{
			    failure(UserModelStatus::kParseError,
			            "Failed to open user model file: " + path.string()),
			};
		}
		return load_document_from_fd(input.get(), path, expectations, false);
	}

	auto UserModelStore::load_document(const std::string           &user,
	                                   const UserModelExpectations &expectations,
	                                   std::optional<uid_t>         owner_uid)
	    -> user_model_codec::Document {
		const auto path_result = resolve(user, false, owner_uid);
		if (path_result.status != UserModelStatus::kOk) {
			return user_model_codec::Document(
			    failure(path_result.status, path_result.error_message));
		}
		return load_document_from_path(path_result.path, expectations);
	}

	auto UserModelStore::inspect(const std::string &user) -> UserModelInspectResult {
		const auto path_result = resolve(user, false, default_secure_owner_uid());
		if (path_result.status != UserModelStatus::kOk) {
			return inspect_failure(path_result.status, path_result.error_message);
		}
		std::string regular_error;
		const auto  regular_status = inspect_regular_file_status(path_result.path, &regular_error);
		if (regular_status != UserModelStatus::kOk) {
			return inspect_failure(regular_status, regular_error);
		}
		const auto current_snapshot = snapshot_model_file(path_result.path);
		if (!current_snapshot.has_value()) {
			return inspect_failure(UserModelStatus::kParseError,
			                       "Failed to inspect user model file: " +
			                           path_result.path.string());
		}
		return UserModelInspectResult{
		    .status   = UserModelStatus::kOk,
		    .snapshot = current_snapshot,
		};
	}

	auto UserModelStore::begin_mutation(const std::string &user) -> UserModelStoreMutationResult {
		const auto path_result = resolve(user, true, default_secure_owner_uid());
		if (path_result.status != UserModelStatus::kOk) {
			return UserModelStoreMutationResult{
			    .document = user_model_codec::Document(
			        failure(path_result.status, path_result.error_message)),
			};
		}
		if (user_model_store_test_hooks::current().before_lock) {
			user_model_store_test_hooks::current().before_lock(path_result.path);
		}
		auto locked_file = open_and_lock_model_file(path_result.path, true);
		if (!locked_file.has_value()) {
			return UserModelStoreMutationResult{
			    .document = user_model_codec::Document(
			        failure(UserModelStatus::kLockFailed, "Failed to lock model file")),
			};
		}
		if (user_model_store_test_hooks::current().after_lock_before_revalidate) {
			user_model_store_test_hooks::current().after_lock_before_revalidate(path_result.path);
		}

		const auto secured_path = resolve(user, true, default_secure_owner_uid());
		if (secured_path.status != UserModelStatus::kOk) {
			return UserModelStoreMutationResult{
			    .document = user_model_codec::Document(
			        failure(secured_path.status, secured_path.error_message)),
			};
		}
		if (!path_identifies_fd(path_result.path, locked_file->lock.fd)) {
			return UserModelStoreMutationResult{
			    .document = user_model_codec::Document(
			        failure(UserModelStatus::kModelChanged,
			                "User model file changed, please rerun the command")),
			};
		}
		auto document =
		    load_document_from_fd(locked_file->lock.fd, path_result.path,
		                          {.backend = {}, .metric = {}, .model = {}, .strict_shape = true},
		                          locked_file->created_empty_file);
		return UserModelStoreMutationResult{
		    .transaction = UserModelStoreTransaction(
		        path_result.path, std::move(locked_file->namespace_lock),
		        std::move(locked_file->lock), locked_file->created_empty_file),
		    .document = std::move(document),
		};
	}

	auto UserModelStore::lock_existing(const std::string &user) -> UserModelStoreTransactionResult {
		const auto path_result = resolve(user, false, default_secure_owner_uid());
		if (path_result.status != UserModelStatus::kOk) {
			return UserModelStoreTransactionResult{
			    .status        = path_result.status,
			    .error_message = path_result.error_message,
			};
		}
		std::string regular_error;
		const auto  regular_status = inspect_regular_file_status(path_result.path, &regular_error);
		if (regular_status != UserModelStatus::kOk) {
			return UserModelStoreTransactionResult{
			    .status        = regular_status,
			    .error_message = regular_error,
			};
		}
		if (user_model_store_test_hooks::current().before_lock) {
			user_model_store_test_hooks::current().before_lock(path_result.path);
		}
		auto locked_file = open_and_lock_model_file(path_result.path, false);
		if (!locked_file.has_value()) {
			return UserModelStoreTransactionResult{
			    .status        = UserModelStatus::kLockFailed,
			    .error_message = "Failed to lock model file",
			};
		}
		if (user_model_store_test_hooks::current().after_lock_before_revalidate) {
			user_model_store_test_hooks::current().after_lock_before_revalidate(path_result.path);
		}

		const auto secured_path = resolve(user, false, default_secure_owner_uid());
		if (secured_path.status != UserModelStatus::kOk) {
			return UserModelStoreTransactionResult{
			    .status        = secured_path.status,
			    .error_message = secured_path.error_message,
			};
		}
		regular_error.clear();
		const auto secured_regular_status =
		    inspect_regular_file_status(path_result.path, &regular_error);
		if (secured_regular_status != UserModelStatus::kOk) {
			return UserModelStoreTransactionResult{
			    .status        = secured_regular_status,
			    .error_message = regular_error,
			};
		}
		if (!path_identifies_fd(path_result.path, locked_file->lock.fd)) {
			return UserModelStoreTransactionResult{
			    .status        = UserModelStatus::kModelChanged,
			    .error_message = "User model file changed, please rerun the command",
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
