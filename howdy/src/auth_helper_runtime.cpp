#include "auth_helper_runtime.hpp"

#include "common/fd_io.hpp"
#include "common/user_names.hpp"
#include "config/config_utils.hpp"
#include "config/runtime_paths.hpp"
#include "storage/user_model_readiness.hpp"

#include <array>
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

#include <acl/libacl.h>

namespace howdy::native::auth_helper {

#ifndef HOWDY_AUTH_HELPER_TESTING
	auto write_all(int fd, const char *data, ssize_t size) -> bool;
	auto select_source_model_path(const std::filesystem::path &source_user_models_dir,
	                              const std::string &user, std::optional<uid_t> owner_uid,
	                              std::optional<std::filesystem::path> &source_model_path) -> bool;
#endif

	namespace {

		constexpr std::size_t kCopyBufferSize = 64 * 1024;

#ifdef HOWDY_AUTH_HELPER_TESTING
		bool            g_fail_acl_setup        = false;
		bool            g_fail_acl_verification = false;
		AclSetFdForTest g_acl_set_fd_for_test   = nullptr;
		AclGetFdForTest g_acl_get_fd_for_test   = nullptr;
		AclResetForTest g_acl_reset_for_test    = nullptr;
#endif

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

		auto validate_runtime_root_for_owner(const std::filesystem::path &path, uid_t owner_uid,
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

		auto secure_source_file_stat_for_owner(int fd, const std::string &label, uid_t owner_uid)
		    -> bool {
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

		enum class AclVerifyStatus {
			kOk,
			kReadError,
			kIterationError,
			kPermissionQueryError,
			kMalformed,
		};

		struct AclVerifyResult {
			AclVerifyStatus  status;
			int              error_number = 0;
			std::string_view operation;
		};

		auto acl_entry_has_permissions(acl_entry_t entry, acl_tag_t tag, const void *qualifier,
		                               int permissions) -> AclVerifyResult {
			acl_tag_t entry_tag;
			if (acl_get_tag_type(entry, &entry_tag) != 0) {
				return {.status       = AclVerifyStatus::kReadError,
				        .error_number = errno,
				        .operation    = "acl_get_tag_type"};
			}
			if (entry_tag != tag) {
				return {.status = AclVerifyStatus::kMalformed};
			}
			if (qualifier != nullptr) {
				void *entry_qualifier = acl_get_qualifier(entry);
				if (entry_qualifier == nullptr) {
					return {.status       = AclVerifyStatus::kReadError,
					        .error_number = errno,
					        .operation    = "acl_get_qualifier"};
				}
				const bool matches = std::memcmp(entry_qualifier, qualifier, sizeof(uid_t)) == 0;
				acl_free(entry_qualifier);
				if (!matches) {
					return {.status = AclVerifyStatus::kMalformed};
				}
			}

			acl_permset_t permission_set;
			if (acl_get_permset(entry, &permission_set) != 0) {
				return {.status       = AclVerifyStatus::kPermissionQueryError,
				        .error_number = errno,
				        .operation    = "acl_get_permset"};
			}
			const int read_result = acl_get_perm(permission_set, ACL_READ);
			if (read_result < 0) {
				return {.status       = AclVerifyStatus::kPermissionQueryError,
				        .error_number = errno,
				        .operation    = "acl_get_perm"};
			}
			const int write_result = acl_get_perm(permission_set, ACL_WRITE);
			if (write_result < 0) {
				return {.status       = AclVerifyStatus::kPermissionQueryError,
				        .error_number = errno,
				        .operation    = "acl_get_perm"};
			}
			const int execute_result = acl_get_perm(permission_set, ACL_EXECUTE);
			if (execute_result < 0) {
				return {.status       = AclVerifyStatus::kPermissionQueryError,
				        .error_number = errno,
				        .operation    = "acl_get_perm"};
			}
			if ((read_result == 1) != ((permissions & ACL_READ) != 0) ||
			    (write_result == 1) != ((permissions & ACL_WRITE) != 0) ||
			    (execute_result == 1) != ((permissions & ACL_EXECUTE) != 0)) {
				return {.status = AclVerifyStatus::kMalformed};
			}
			return {.status = AclVerifyStatus::kOk};
		}

		auto get_fd_acl(int fd) -> acl_t {
#ifdef HOWDY_AUTH_HELPER_TESTING
			if (g_acl_get_fd_for_test != nullptr) {
				return g_acl_get_fd_for_test(fd);
			}
#endif
			return acl_get_fd(fd);
		}

		auto set_fd_acl(int fd, acl_t acl) -> int {
#ifdef HOWDY_AUTH_HELPER_TESTING
			if (g_acl_set_fd_for_test != nullptr) {
				return g_acl_set_fd_for_test(fd, acl);
			}
#endif
			return acl_set_fd(fd, acl);
		}

		auto verify_private_acl(int fd, uid_t uid, bool directory) -> AclVerifyResult {
#ifdef HOWDY_AUTH_HELPER_TESTING
			if (g_fail_acl_verification) {
				return {.status = AclVerifyStatus::kMalformed};
			}
#endif
			acl_t acl = get_fd_acl(fd);
			if (acl == nullptr) {
				return {.status       = AclVerifyStatus::kReadError,
				        .error_number = errno,
				        .operation    = "acl_get_fd"};
			}

			const int            permissions   = directory ? (ACL_READ | ACL_EXECUTE) : ACL_READ;
			constexpr std::array kExpectedTags = {ACL_USER_OBJ, ACL_USER, ACL_GROUP_OBJ, ACL_MASK,
			                                      ACL_OTHER};
			int                  entry_count   = 0;
			acl_entry_t          entry;
			int                  entry_id = ACL_FIRST_ENTRY;
			while (true) {
				const int entry_result = acl_get_entry(acl, entry_id, &entry);
				if (entry_result < 0) {
					const int error_number = errno;
					acl_free(acl);
					return {.status       = AclVerifyStatus::kIterationError,
					        .error_number = error_number,
					        .operation    = "acl_get_entry"};
				}
				if (entry_result == 0) {
					break;
				}
				entry_id = ACL_NEXT_ENTRY;
				if (entry_count >= 5) {
					acl_free(acl);
					return {.status = AclVerifyStatus::kMalformed};
				}
				acl_tag_t tag;
				if (acl_get_tag_type(entry, &tag) != 0) {
					const int error_number = errno;
					acl_free(acl);
					return {.status       = AclVerifyStatus::kReadError,
					        .error_number = error_number,
					        .operation    = "acl_get_tag_type"};
				}
				if (tag != kExpectedTags[entry_count]) {
					acl_free(acl);
					return {.status = AclVerifyStatus::kMalformed};
				}
				++entry_count;
				const auto result = acl_entry_has_permissions(
				    entry, tag, tag == ACL_USER ? static_cast<const void *>(&uid) : nullptr,
				    tag == ACL_GROUP_OBJ || tag == ACL_OTHER ? 0 : permissions);
				if (result.status != AclVerifyStatus::kOk) {
					acl_free(acl);
					return result;
				}
			}
			acl_free(acl);
			if (entry_count != 5) {
				return {.status = AclVerifyStatus::kMalformed};
			}
			return {.status = AclVerifyStatus::kOk};
		}

		auto log_acl_verify_failure(const std::filesystem::path &path,
		                            const AclVerifyResult       &result) -> bool {
			if (result.status == AclVerifyStatus::kMalformed) {
				std::cerr << "ACL policy mismatch for staged object '" << path << "'\n";
				return false;
			}
			return log_errno_failure(std::string(result.operation) + " while verifying ACL", path,
			                         result.error_number);
		}

		auto set_private_acl(int fd, const std::filesystem::path &path, uid_t uid, bool directory)
		    -> bool {
#ifdef HOWDY_AUTH_HELPER_TESTING
			if (g_fail_acl_setup) {
				errno                  = EIO;
				const int error_number = errno;
				return log_errno_failure("apply ACL to staged object", path, error_number);
			}
#endif
			const int permissions = directory ? (ACL_READ | ACL_EXECUTE) : ACL_READ;
			acl_t     acl         = acl_init(5);
			if (acl == nullptr) {
				const int error_number = errno;
				return log_errno_failure("allocate ACL for staged object", path, error_number);
			}

			const auto add_entry = [&acl, &path](acl_tag_t tag, const void *qualifier,
			                                     int entry_permissions) {
				acl_entry_t   entry;
				acl_permset_t permission_set;
				if (acl_create_entry(&acl, &entry) != 0) {
					const int error_number = errno;
					return log_errno_failure("acl_create_entry for staged object", path,
					                         error_number);
				}
				if (acl_set_tag_type(entry, tag) != 0) {
					const int error_number = errno;
					return log_errno_failure("acl_set_tag_type for staged object", path,
					                         error_number);
				}
				if (qualifier != nullptr) {
					if (acl_set_qualifier(entry, qualifier) != 0) {
						const int error_number = errno;
						return log_errno_failure("acl_set_qualifier for staged object", path,
						                         error_number);
					}
				}
				if (acl_get_permset(entry, &permission_set) != 0) {
					const int error_number = errno;
					return log_errno_failure("acl_get_permset for staged object", path,
					                         error_number);
				}
				if (acl_clear_perms(permission_set) != 0) {
					const int error_number = errno;
					return log_errno_failure("acl_clear_perms for staged object", path,
					                         error_number);
				}
				for (const auto permission : {ACL_READ, ACL_WRITE, ACL_EXECUTE}) {
					if ((entry_permissions & permission) == 0) {
						continue;
					}
					if (acl_add_perm(permission_set, permission) != 0) {
						const int error_number = errno;
						return log_errno_failure("acl_add_perm for staged object", path,
						                         error_number);
					}
				}
				if (acl_set_permset(entry, permission_set) != 0) {
					const int error_number = errno;
					return log_errno_failure("acl_set_permset for staged object", path,
					                         error_number);
				}
				return true;
			};

			if (!add_entry(ACL_USER_OBJ, nullptr, permissions)) {
				acl_free(acl);
				return false;
			}
			if (!add_entry(ACL_USER, &uid, permissions)) {
				acl_free(acl);
				return false;
			}
			if (!add_entry(ACL_GROUP_OBJ, nullptr, 0)) {
				acl_free(acl);
				return false;
			}
			if (!add_entry(ACL_MASK, nullptr, permissions)) {
				acl_free(acl);
				return false;
			}
			if (!add_entry(ACL_OTHER, nullptr, 0)) {
				acl_free(acl);
				return false;
			}
			if (acl_valid(acl) != 0) {
				const int error_number = errno;
				acl_free(acl);
				return log_errno_failure("acl_valid for staged object", path, error_number);
			}
			if (set_fd_acl(fd, acl) != 0) {
				const int error_number = errno;
				acl_free(acl);
				return log_errno_failure("acl_set_fd for staged object", path, error_number);
			}
			acl_free(acl);
			const auto verification = verify_private_acl(fd, uid, directory);
			if (verification.status != AclVerifyStatus::kOk) {
				return log_acl_verify_failure(path, verification);
			}
			return true;
		}

		auto secure_runtime_fd(int fd, const std::filesystem::path &path, uid_t uid,
		                       uid_t owner_uid, gid_t owner_gid, bool directory) -> bool {
			const mode_t mode = directory ? 0500 : 0400;
			if (!fchown_if_needed(fd, path, owner_uid, owner_gid)) {
				return false;
			}
			if (fchmod(fd, mode) != 0) {
				const int error_number = errno;
				return log_errno_failure("fchmod staged object", path, error_number);
			}
			return set_private_acl(fd, path, uid, directory);
		}

		auto finalize_runtime_directory(const std::filesystem::path &path, uid_t uid,
		                                uid_t owner_uid, gid_t owner_gid) -> bool {
			const int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
			if (fd < 0) {
				const int error_number = errno;
				return log_errno_failure("open staged directory", path, error_number);
			}
			if (!secure_runtime_fd(fd, path, uid, owner_uid, owner_gid, true)) {
				close(fd);
				return false;
			}
			if (close(fd) != 0) {
				const int error_number = errno;
				return log_errno_failure("close staged directory", path, error_number);
			}
			return true;
		}

		auto copy_file_for_owner(const std::filesystem::path &source,
		                         const std::filesystem::path &destination, const std::string &label,
		                         uid_t uid, uid_t owner_uid, gid_t owner_gid) -> bool {
			const int source_fd = open(source.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
			if (source_fd < 0) {
				std::cerr << "Failed to open " << label << ": " << source << " ("
				          << std::strerror(errno) << ")\n";
				return false;
			}

			if (!secure_source_file_stat_for_owner(source_fd, label, owner_uid)) {
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
			    secure_runtime_fd(destination_fd, destination, uid, owner_uid, owner_gid, false);
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

		auto make_private_runtime_dir(const std::filesystem::path &root, uid_t uid, uid_t owner_uid,
		                              gid_t root_gid) -> std::optional<std::filesystem::path> {
			if (!validate_runtime_root_for_owner(root, owner_uid, root_gid)) {
				return std::nullopt;
			}

			std::string       templ = (root / ("pam-" + std::to_string(uid) + "-XXXXXX")).string();
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
			if (!fchown_if_needed(fd, path, owner_uid, root_gid)) {
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

		auto make_user_models_dir(const PreparedPaths &prepared, uid_t owner_uid, gid_t owner_gid)
		    -> bool {
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
			if (!fchown_if_needed(fd, prepared.user_models_dir, owner_uid, owner_gid)) {
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
		                           const std::filesystem::path &source_config, uid_t uid,
		                           uid_t owner_uid, gid_t owner_gid) -> bool {
			const auto config_security =
			    howdy::native::check_secure_config_path(source_config, owner_uid);
			if (!config_security.ok) {
				std::cerr << config_security.error_message << "\n";
				return false;
			}
			return copy_file_for_owner(source_config, prepared.config_path, "Config file", uid,
			                           owner_uid, owner_gid);
		}

		auto stage_user_model_for_user(const std::string &user, const PreparedPaths &prepared,
		                               const std::filesystem::path &source_user_models_dir,
		                               uid_t uid, uid_t owner_uid, gid_t owner_gid) -> bool {
			std::optional<std::filesystem::path> source_model_path;
			if (!select_source_model_path(source_user_models_dir, user, owner_uid,
			                              source_model_path)) {
				return false;
			}
			if (!source_model_path.has_value()) {
				return true;
			}
			return copy_file_for_owner(*source_model_path,
			                           prepared.user_models_dir / source_model_path->filename(),
			                           "User model file", uid, owner_uid, owner_gid);
		}

		auto prepare_runtime_auth_files_from(const std::string &user, uid_t uid,
		                                     const std::filesystem::path &runtime_root,
		                                     const std::filesystem::path &source_config,
		                                     const std::filesystem::path &source_user_models_dir,
		                                     uid_t owner_uid, gid_t root_gid)
		    -> std::optional<PreparedPaths> {
			auto runtime_dir = make_private_runtime_dir(runtime_root, uid, owner_uid, root_gid);
			if (!runtime_dir.has_value()) {
				return std::nullopt;
			}

			RuntimeDirGuard guard(*runtime_dir);
			PreparedPaths   prepared{
			    .runtime_dir     = guard.path(),
			    .config_path     = guard.path() / "config.ini",
			    .user_models_dir = guard.path() / "models",
			};

			if (!make_user_models_dir(prepared, owner_uid, root_gid) ||
			    !stage_config_for_user(prepared, source_config, uid, owner_uid, root_gid) ||
			    !stage_user_model_for_user(user, prepared, source_user_models_dir, uid, owner_uid,
			                               root_gid) ||
			    !finalize_runtime_directory(prepared.user_models_dir, uid, owner_uid, root_gid) ||
			    !finalize_runtime_directory(prepared.runtime_dir, uid, owner_uid, root_gid)) {
				return std::nullopt;
			}

			guard.release();
			return prepared;
		}

		auto cleanup_runtime_auth_files_from(const std::filesystem::path &path, uid_t uid,
		                                     const std::filesystem::path &root)
		    -> CleanupRuntimeResult {
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

	}  // namespace

	auto runtime_root() -> std::filesystem::path {
		return "/run/howdy";
	}

	auto validate_runtime_root(const std::filesystem::path &path) -> bool {
		return validate_runtime_root_for_owner(path, 0, 0);
	}

	auto secure_source_file_stat(int fd, const std::string &label) -> bool {
		return secure_source_file_stat_for_owner(fd, label, 0);
	}

	auto write_all(int fd, const char *data, ssize_t size) -> bool {
		if (size <= 0) {
			return true;
		}
		return howdy::native::write_all_to_fd(fd, data, static_cast<std::size_t>(size));
	}

	auto copy_file_for_user(const std::filesystem::path &source,
	                        const std::filesystem::path &destination, const std::string &label,
	                        gid_t invoking_gid) -> bool {
		(void)invoking_gid;
		return copy_file_for_owner(source, destination, label, geteuid(), 0, 0);
	}

	auto select_source_model_path(const std::filesystem::path &source_user_models_dir,
	                              const std::string &user, std::optional<uid_t> owner_uid,
	                              std::optional<std::filesystem::path> &source_model_path) -> bool {
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
				std::cerr << (readiness.error_message.empty() ? "Failed to validate user model file"
				                                              : readiness.error_message)
				          << "\n";
				return false;
		}
	}

	auto prepare_runtime_auth_files(const std::string &user, uid_t uid, gid_t invoking_gid)
	    -> std::optional<PreparedPaths> {
		(void)invoking_gid;
		return prepare_runtime_auth_files_from(user, uid, runtime_root(),
		                                       howdy::native::resolve_config_path(),
		                                       howdy::native::resolve_user_models_dir(), 0, 0);
	}

	auto cleanup_runtime_auth_files(const std::filesystem::path &path, uid_t uid,
	                                gid_t invoking_gid) -> CleanupRuntimeResult {
		(void)invoking_gid;
		return cleanup_runtime_auth_files_from(path, uid, runtime_root());
	}

#ifdef HOWDY_AUTH_HELPER_TESTING
	auto cleanup_runtime_auth_files_for_test(const std::filesystem::path &path, uid_t uid,
	                                         gid_t                        invoking_gid,
	                                         const std::filesystem::path &runtime_root)
	    -> CleanupRuntimeResult {
		(void)invoking_gid;
		return cleanup_runtime_auth_files_from(path, uid, runtime_root);
	}

	auto prepare_runtime_auth_files_for_test(const std::string &user, uid_t uid, gid_t invoking_gid,
	                                         const std::filesystem::path &runtime_root,
	                                         const std::filesystem::path &source_config,
	                                         const std::filesystem::path &source_user_models_dir,
	                                         uid_t owner_uid) -> std::optional<PreparedPaths> {
		(void)invoking_gid;
		return prepare_runtime_auth_files_from(user, uid, runtime_root, source_config,
		                                       source_user_models_dir, owner_uid, getegid());
	}

	auto set_acl_setup_failure_for_test(bool fail) -> void {
		g_fail_acl_setup = fail;
	}

	auto set_acl_verification_failure_for_test(bool fail) -> void {
		g_fail_acl_verification = fail;
	}

	auto set_acl_io_for_test(AclSetFdForTest set_fd, AclGetFdForTest get_fd, AclResetForTest reset)
	    -> void {
		reset_acl_io_for_test();
		g_acl_set_fd_for_test = set_fd;
		g_acl_get_fd_for_test = get_fd;
		g_acl_reset_for_test  = reset;
	}

	auto reset_acl_io_for_test() -> void {
		if (g_acl_reset_for_test != nullptr) {
			g_acl_reset_for_test();
		}
		g_acl_set_fd_for_test = nullptr;
		g_acl_get_fd_for_test = nullptr;
		g_acl_reset_for_test  = nullptr;
	}
#endif

}  // namespace howdy::native::auth_helper
