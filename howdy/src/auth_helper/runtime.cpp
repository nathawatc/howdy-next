#include "auth_helper/acl.hpp"
#include "auth_helper/runtime_internal.hpp"
#include "config/config_utils.hpp"
#include "config/runtime_paths.hpp"
#include "storage/user_model_readiness.hpp"
#include "support/fd_io.hpp"
#include "support/user_names.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

#include <sys/stat.h>

namespace howdy::native::auth_helper {
	namespace {

		constexpr std::size_t kCopyBufferSize = std::size_t{64} * 1024;

		using internal::RuntimeSources;
		using internal::StagedIdentity;

		auto log_errno_failure(std::string_view operation, const std::filesystem::path &path,
		                       int error_number) -> bool;

		class RuntimeDirGuard {
		public:
			explicit RuntimeDirGuard(std::filesystem::path path)
			    : path_(std::move(path)) {}

			~RuntimeDirGuard() {
				if (active_) {
					std::error_code ec;
					std::filesystem::remove_all(path_, ec);
				}
			}

			RuntimeDirGuard(const RuntimeDirGuard &)                     = delete;
			auto operator=(const RuntimeDirGuard &) -> RuntimeDirGuard & = delete;

			void release() {
				active_ = false;
			}

			[[nodiscard]] auto path() const -> const std::filesystem::path & {
				return path_;
			}

		private:
			std::filesystem::path path_;
			bool                  active_ = true;
		};

	}  // namespace

	namespace internal {
		auto validate_runtime_root(const std::filesystem::path &path, uid_t owner_uid,
		                           gid_t owner_gid) -> bool {
			const bool created = mkdir(path.c_str(), 0711) == 0;
			if (!created && errno != EEXIST) {
				std::cerr << "Failed to create runtime directory: " << path << " ("
				          << std::strerror(errno) << ")\n";
				return false;
			}

			if (created && chown(path.c_str(), owner_uid, owner_gid) != 0) {
				const int error_number = errno;
				return log_errno_failure("chown runtime directory", path, error_number);
			}
			if (created && chmod(path.c_str(), 0711) != 0) {
				const int error_number = errno;
				return log_errno_failure("chmod runtime directory", path, error_number);
			}

			struct stat stat_{};
			if (lstat(path.c_str(), &stat_) != 0) {
				std::cerr << "Failed to inspect runtime directory: " << path << " ("
				          << std::strerror(errno) << ")\n";
				return false;
			}

			if (!S_ISDIR(stat_.st_mode)) {
				std::cerr << "Runtime path is not a directory: " << path << "\n";
				return false;
			}

			if (stat_.st_uid != owner_uid || stat_.st_gid != owner_gid ||
			    (stat_.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
				std::cerr << "Runtime directory is not controlled by expected owner: " << path
				          << "\n";
				return false;
			}

			if (chmod(path.c_str(), 0711) != 0) {
				const int error_number = errno;
				return log_errno_failure("chmod runtime directory", path, error_number);
			}

			return true;
		}

	}  // namespace internal

	namespace {
		auto log_errno_failure(std::string_view operation, const std::filesystem::path &path,
		                       int error_number) -> bool {
			std::cerr << "Failed to " << operation << " '" << path
			          << "': " << std::strerror(error_number) << "\n";
			return false;
		}

		auto fchown_if_needed(int fd, const std::filesystem::path &path, uid_t uid, gid_t gid)
		    -> bool {
			struct stat stat_{};
			if (fstat(fd, &stat_) != 0) {
				const int error_number = errno;
				return log_errno_failure("fstat staged object", path, error_number);
			}
			if (stat_.st_uid == uid && stat_.st_gid == gid) {
				return true;
			}
			if (fchown(fd, uid, gid) == 0) {
				return true;
			}
			const int error_number = errno;
			return log_errno_failure("fchown staged object", path, error_number);
		}

	}  // namespace

	namespace internal {
		auto secure_source_file_stat(int fd, const std::string &label, uid_t owner_uid) -> bool {
			struct stat stat_{};
			if (fstat(fd, &stat_) != 0) {
				std::cerr << "Failed to inspect " << label << ": " << std::strerror(errno) << "\n";
				return false;
			}

			if (!S_ISREG(stat_.st_mode) || stat_.st_uid != owner_uid ||
			    (stat_.st_mode & (S_IWGRP | S_IWOTH)) != 0 || stat_.st_nlink != 1) {
				std::cerr << label << " failed secure file validation\n";
				return false;
			}

			return true;
		}
	}  // namespace internal

	namespace {
		auto secure_runtime_fd(int fd, const std::filesystem::path &path, StagedIdentity identity,
		                       bool directory, const AclOperations &operations) -> bool {
			const mode_t mode = directory ? 0500 : 0400;
			if (!fchown_if_needed(fd, path, identity.owner_uid, identity.owner_gid)) {
				return false;
			}
			if (fchmod(fd, mode) != 0) {
				const int error_number = errno;
				return log_errno_failure("fchmod staged object", path, error_number);
			}
			return set_private_acl_with_operations(fd, path, identity.target_uid, directory,
			                                       operations);
		}

		auto finalize_runtime_directory(const std::filesystem::path &path, StagedIdentity identity,
		                                const AclOperations &operations) -> bool {
			const int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
			if (fd < 0) {
				const int error_number = errno;
				return log_errno_failure("open staged directory", path, error_number);
			}
			if (!secure_runtime_fd(fd, path, identity, true, operations)) {
				close(fd);
				return false;
			}
			if (close(fd) != 0) {
				const int error_number = errno;
				return log_errno_failure("close staged directory", path, error_number);
			}
			return true;
		}

	}  // namespace

	namespace internal {
		auto copy_file(const std::filesystem::path &source,
		               const std::filesystem::path &destination, const std::string &label,
		               StagedIdentity identity, const AclOperations &operations) -> bool {
			const int source_fd = open(source.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
			if (source_fd < 0) {
				std::cerr << "Failed to open " << label << ": " << source << " ("
				          << std::strerror(errno) << ")\n";
				return false;
			}

			if (!secure_source_file_stat(source_fd, label, identity.owner_uid)) {
				close(source_fd);
				return false;
			}

			const int destination_fd = open(
			    destination.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
			if (destination_fd < 0) {
				std::cerr << "Failed to create runtime " << label << ": " << destination << " ("
				          << std::strerror(errno) << ")\n";
				close(source_fd);
				return false;
			}

			bool              ok = true;
			std::vector<char> buffer(kCopyBufferSize);
			while (true) {
				const ssize_t read_size = read(source_fd, buffer.data(), buffer.size());
				if (read_size < 0 && errno == EINTR) {
					continue;
				}
				if (read_size < 0) {
					ok = false;
					break;
				}
				if (read_size == 0) {
					break;
				}
				if (!write_all(destination_fd, buffer.data(), read_size)) {
					ok = false;
					break;
				}
			}

			const bool secured =
			    secure_runtime_fd(destination_fd, destination, identity, false, operations);
			if (!secured) {
				ok = false;
			}
			if (close(destination_fd) != 0) {
				const int error_number = errno;
				log_errno_failure("close staged file", destination, error_number);
				ok = false;
			}
			if (close(source_fd) != 0) {
				const int error_number = errno;
				log_errno_failure("close source file", source, error_number);
				ok = false;
			}
			return ok;
		}
	}  // namespace internal

	namespace {
		auto make_private_runtime_dir(const std::filesystem::path &root, StagedIdentity identity)
		    -> std::optional<std::filesystem::path> {
			if (!internal::validate_runtime_root(root, identity.owner_uid, identity.owner_gid)) {
				return std::nullopt;
			}

			std::string templ =
			    (root / ("pam-" + std::to_string(identity.target_uid) + "-XXXXXX")).string();
			std::vector<char> buffer(templ.begin(), templ.end());
			buffer.push_back('\0');

			char *created = mkdtemp(buffer.data());
			if (created == nullptr) {
				const int error_number = errno;
				log_errno_failure("mkdtemp private runtime directory", root, error_number);
				return std::nullopt;
			}

			std::filesystem::path path(created);
			const int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
			if (fd < 0) {
				const int error_number = errno;
				log_errno_failure("open private runtime directory", path, error_number);
				std::error_code ec;
				std::filesystem::remove_all(path, ec);
				return std::nullopt;
			}
			if (!fchown_if_needed(fd, path, identity.owner_uid, identity.owner_gid)) {
				close(fd);
				std::error_code ec;
				std::filesystem::remove_all(path, ec);
				return std::nullopt;
			}
			if (fchmod(fd, 0700) != 0) {
				const int error_number = errno;
				log_errno_failure("fchmod private runtime directory", path, error_number);
				close(fd);
				std::error_code ec;
				std::filesystem::remove_all(path, ec);
				return std::nullopt;
			}
			if (close(fd) != 0) {
				const int error_number = errno;
				log_errno_failure("close private runtime directory", path, error_number);
				std::error_code ec;
				std::filesystem::remove_all(path, ec);
				return std::nullopt;
			}
			return path;
		}

		auto make_user_models_dir(const PreparedPaths &prepared, StagedIdentity identity) -> bool {
			if (mkdir(prepared.user_models_dir.c_str(), 0700) != 0) {
				const int error_number = errno;
				return log_errno_failure("mkdir runtime user models directory",
				                         prepared.user_models_dir, error_number);
			}
			const int fd = open(prepared.user_models_dir.c_str(),
			                    O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
			if (fd < 0) {
				const int error_number = errno;
				return log_errno_failure("open runtime user models directory",
				                         prepared.user_models_dir, error_number);
			}
			if (!fchown_if_needed(fd, prepared.user_models_dir, identity.owner_uid,
			                      identity.owner_gid)) {
				close(fd);
				return false;
			}
			if (fchmod(fd, 0700) != 0) {
				const int error_number = errno;
				log_errno_failure("fchmod runtime user models directory", prepared.user_models_dir,
				                  error_number);
				close(fd);
				return false;
			}
			if (close(fd) != 0) {
				const int error_number = errno;
				return log_errno_failure("close runtime user models directory",
				                         prepared.user_models_dir, error_number);
			}
			return true;
		}

		auto stage_config_for_user(const PreparedPaths         &prepared,
		                           const std::filesystem::path &source_config,
		                           StagedIdentity identity, const AclOperations &operations)
		    -> bool {
			const auto config_security =
			    howdy::native::check_secure_config_path(source_config, identity.owner_uid);
			if (!config_security.ok) {
				std::cerr << config_security.error_message << "\n";
				return false;
			}
			return internal::copy_file(source_config, prepared.config_path, "Config file", identity,
			                           operations);
		}

		auto stage_user_model_for_user(const std::string &user, const PreparedPaths &prepared,
		                               const std::filesystem::path &source_user_models_dir,
		                               StagedIdentity identity, const AclOperations &operations)
		    -> bool {
			std::optional<std::filesystem::path> source_model_path;
			if (!internal::select_source_model_path(source_user_models_dir, user,
			                                        identity.owner_uid, source_model_path)) {
				return false;
			}
			if (!source_model_path.has_value()) {
				return true;
			}
			return internal::copy_file(*source_model_path,
			                           prepared.user_models_dir / source_model_path->filename(),
			                           "User model file", identity, operations);
		}

	}  // namespace

	namespace internal {
		auto prepare_runtime_auth_files(const std::string &user, StagedIdentity identity,
		                                const RuntimeSources &sources,
		                                const AclOperations  &operations)
		    -> std::optional<PreparedPaths> {
			auto runtime_dir = make_private_runtime_dir(sources.runtime_root, identity);
			if (!runtime_dir.has_value()) {
				return std::nullopt;
			}

			RuntimeDirGuard guard(*runtime_dir);
			PreparedPaths   prepared{
			    .runtime_dir     = guard.path(),
			    .config_path     = guard.path() / "config.ini",
			    .user_models_dir = guard.path() / "models",
			};

			if (!make_user_models_dir(prepared, identity) ||
			    !stage_config_for_user(prepared, sources.config, identity, operations) ||
			    !stage_user_model_for_user(user, prepared, sources.user_models_dir, identity,
			                               operations) ||
			    !finalize_runtime_directory(prepared.user_models_dir, identity, operations) ||
			    !finalize_runtime_directory(prepared.runtime_dir, identity, operations)) {
				return std::nullopt;
			}

			guard.release();
			return prepared;
		}

		auto cleanup_runtime_auth_files(const std::filesystem::path &path, uid_t uid,
		                                const std::filesystem::path &root) -> CleanupRuntimeResult {
			const auto expected_prefix = "pam-" + std::to_string(uid) + "-";
			if (path.parent_path() != root ||
			    !path.filename().string().starts_with(expected_prefix)) {
				return {.ok            = false,
				        .error_message = "Refusing to clean unexpected runtime directory"};
			}

			struct stat stat_{};
			if (lstat(path.c_str(), &stat_) != 0) {
				if (errno == ENOENT) {
					return {.ok = true};
				}
				return {.ok            = false,
				        .error_message = "Failed to inspect runtime directory for cleanup"};
			}

			if (!S_ISDIR(stat_.st_mode) || stat_.st_uid != 0 || stat_.st_gid != 0 ||
			    (stat_.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
				return {.ok            = false,
				        .error_message = "Refusing to clean insecure runtime directory"};
			}

			std::error_code ec;
			std::filesystem::remove_all(path, ec);
			if (ec) {
				return {.ok            = false,
				        .error_message = "Failed to clean runtime directory: " + ec.message()};
			}
			return {.ok = true};
		}

	}  // namespace internal

	auto runtime_root() -> std::filesystem::path {
		return "/run/howdy";
	}

	namespace internal {
		auto write_all(int fd, const char *data, ssize_t size) -> bool {
			if (size <= 0) {
				return true;
			}
			return howdy::native::write_all_to_fd(fd, data, static_cast<std::size_t>(size));
		}

		auto select_source_model_path(const std::filesystem::path &source_user_models_dir,
		                              const std::string &user, std::optional<uid_t> owner_uid,
		                              std::optional<std::filesystem::path> &source_model_path)
		    -> bool {
			source_model_path.reset();
			const auto readiness =
			    howdy::native::check_user_model_readiness(source_user_models_dir, user, owner_uid);
			switch (readiness.status) {
				case howdy::native::UserModelStatus::kOk:
					source_model_path = readiness.path;
					return true;
				case howdy::native::UserModelStatus::kNoModel:
				case howdy::native::UserModelStatus::kNoModelDirectory:
					return true;
				case howdy::native::UserModelStatus::kInvalidUser:
					std::cerr << howdy::native::kInvalidUserNameMessage << "\n";
					return false;
				default:
					std::cerr << (readiness.error_message.empty()
					                  ? "Failed to validate user model file"
					                  : readiness.error_message)
					          << "\n";
					return false;
			}
		}
	}  // namespace internal

	auto prepare_runtime_auth_files(const std::string &user, RuntimeIdentity identity)
	    -> std::optional<PreparedPaths> {
		(void)identity.gid;
		return internal::prepare_runtime_auth_files(
		    user, {.target_uid = identity.uid, .owner_uid = 0, .owner_gid = 0},
		    {.runtime_root    = runtime_root(),
		     .config          = howdy::native::resolve_config_path(),
		     .user_models_dir = howdy::native::resolve_user_models_dir()},
		    production_acl_operations());
	}

	auto cleanup_runtime_auth_files(const std::filesystem::path &path, RuntimeIdentity identity)
	    -> CleanupRuntimeResult {
		(void)identity.gid;
		return internal::cleanup_runtime_auth_files(path, identity.uid, runtime_root());
	}

}  // namespace howdy::native::auth_helper
