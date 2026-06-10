#include "auth_helper_runtime.hpp"

#include "common/user_names.hpp"
#include "config/config_utils.hpp"
#include "config/runtime_paths.hpp"
#include "storage/user_model_readiness.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

#include <sys/stat.h>

namespace howdy::native::auth_helper {

#ifndef HOWDY_AUTH_HELPER_TESTING
	auto write_all(int fd, const char *data, ssize_t size) -> bool;
	auto select_source_model_path(const std::filesystem::path &source_user_models_dir,
	                              const std::string &user, std::optional<uid_t> owner_uid,
	                              std::optional<std::filesystem::path> &source_model_path) -> bool;
#endif

	namespace {

		constexpr std::size_t kCopyBufferSize = 64 * 1024;

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

			if (created && (chown(path.c_str(), owner_uid, owner_gid) != 0 ||
			                chmod(path.c_str(), 0711) != 0)) {
				std::cerr << "Failed to secure runtime directory: " << path << " ("
				          << std::strerror(errno) << ")\n";
				return false;
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
				std::cerr << "Runtime directory is not controlled by the expected owner: " << path
				          << "\n";
				return false;
			}

			if (chmod(path.c_str(), 0711) != 0) {
				std::cerr << "Failed to secure runtime directory: " << path << " ("
				          << std::strerror(errno) << ")\n";
				return false;
			}

			return true;
		}

		auto chown_if_needed(const std::filesystem::path &path, uid_t uid, gid_t gid) -> bool {
			struct stat stat_{};
			if (lstat(path.c_str(), &stat_) != 0) {
				return false;
			}
			if (stat_.st_uid == uid && stat_.st_gid == gid) {
				return true;
			}
			return chown(path.c_str(), uid, gid) == 0;
		}

		auto fchown_if_needed(int fd, uid_t uid, gid_t gid) -> bool {
			struct stat stat_{};
			if (fstat(fd, &stat_) != 0) {
				return false;
			}
			if (stat_.st_uid == uid && stat_.st_gid == gid) {
				return true;
			}
			return fchown(fd, uid, gid) == 0;
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

		auto copy_file_for_owner(const std::filesystem::path &source,
		                         const std::filesystem::path &destination, const std::string &label,
		                         gid_t gid, uid_t owner_uid) -> bool {
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

			const int destination_fd =
			    open(destination.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
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

			if (!fchown_if_needed(destination_fd, owner_uid, gid) ||
			    fchmod(destination_fd, 0440) != 0) {
				ok = false;
			}

			close(destination_fd);
			close(source_fd);
			return ok;
		}

		auto make_private_runtime_dir(const std::filesystem::path &root, uid_t uid, gid_t gid,
		                              uid_t owner_uid, gid_t root_gid)
		    -> std::optional<std::filesystem::path> {
			if (!validate_runtime_root_for_owner(root, owner_uid, root_gid)) {
				return std::nullopt;
			}

			std::string       templ = (root / ("pam-" + std::to_string(uid) + "-XXXXXX")).string();
			std::vector<char> buffer(templ.begin(), templ.end());
			buffer.push_back('\0');

			char *created = mkdtemp(buffer.data());
			if (created == nullptr) {
				std::cerr << "Failed to create private runtime directory: " << std::strerror(errno)
				          << "\n";
				return std::nullopt;
			}

			std::filesystem::path path(created);
			const mode_t          private_dir_mode = owner_uid == 0 ? 0550 : 0750;
			if (!chown_if_needed(path, owner_uid, gid) ||
			    chmod(path.c_str(), private_dir_mode) != 0) {
				std::cerr << "Failed to secure private runtime directory: " << std::strerror(errno)
				          << "\n";
				std::error_code ec;
				std::filesystem::remove_all(path, ec);
				return std::nullopt;
			}

			return path;
		}

		auto make_user_models_dir(const PreparedPaths &prepared, gid_t gid, uid_t owner_uid)
		    -> bool {
			std::error_code ec;
			std::filesystem::create_directory(prepared.user_models_dir, ec);
			const mode_t user_models_dir_mode = owner_uid == 0 ? 0550 : 0750;
			if (ec || !chown_if_needed(prepared.user_models_dir, owner_uid, gid) ||
			    chmod(prepared.user_models_dir.c_str(), user_models_dir_mode) != 0) {
				std::cerr << "Failed to create runtime user models directory\n";
				return false;
			}
			return true;
		}

		auto stage_config_for_user(const PreparedPaths         &prepared,
		                           const std::filesystem::path &source_config, gid_t gid,
		                           uid_t owner_uid) -> bool {
			const auto config_security =
			    howdy::native::check_secure_config_path(source_config, owner_uid);
			if (!config_security.ok) {
				std::cerr << config_security.error_message << "\n";
				return false;
			}

			return copy_file_for_owner(source_config, prepared.config_path, "Config file", gid,
			                           owner_uid);
		}

		auto stage_user_model_for_user(const std::string &user, const PreparedPaths &prepared,
		                               const std::filesystem::path &source_user_models_dir,
		                               gid_t gid, uid_t owner_uid) -> bool {
			std::optional<std::filesystem::path> source_model_path;
			if (!select_source_model_path(source_user_models_dir, user, owner_uid,
			                              source_model_path)) {
				return false;
			}

			if (!source_model_path.has_value()) {
				return true;
			}

			const auto runtime_model_path =
			    prepared.user_models_dir / source_model_path->filename();
			return copy_file_for_owner(*source_model_path, runtime_model_path, "User model file",
			                           gid, owner_uid);
		}

		auto prepare_runtime_auth_files_from(const std::string &user, uid_t uid, gid_t gid,
		                                     const std::filesystem::path &runtime_root,
		                                     const std::filesystem::path &source_config,
		                                     const std::filesystem::path &source_user_models_dir,
		                                     uid_t owner_uid, gid_t root_gid)
		    -> std::optional<PreparedPaths> {
			auto runtime_dir =
			    make_private_runtime_dir(runtime_root, uid, gid, owner_uid, root_gid);
			if (!runtime_dir.has_value()) {
				return std::nullopt;
			}

			RuntimeDirGuard guard(*runtime_dir);
			PreparedPaths   prepared{
			    .runtime_dir     = guard.path(),
			    .config_path     = guard.path() / "config.ini",
			    .user_models_dir = guard.path() / "models",
			};

			if (!make_user_models_dir(prepared, gid, owner_uid) ||
			    !stage_config_for_user(prepared, source_config, gid, owner_uid) ||
			    !stage_user_model_for_user(user, prepared, source_user_models_dir, gid,
			                               owner_uid)) {
				return std::nullopt;
			}

			guard.release();
			return prepared;
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
		ssize_t written = 0;
		while (written < size) {
			const ssize_t result =
			    write(fd, data + written, static_cast<std::size_t>(size - written));
			if (result < 0 && errno == EINTR) {
				continue;
			}
			if (result <= 0) {
				return false;
			}
			written += result;
		}
		return true;
	}

	auto copy_file_for_user(const std::filesystem::path &source,
	                        const std::filesystem::path &destination, const std::string &label,
	                        gid_t gid) -> bool {
		return copy_file_for_owner(source, destination, label, gid, 0);
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
				// Missing source storage means prepare continues without staging a model; compare
				// later decides whether authentication is unavailable.
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

	auto prepare_runtime_auth_files(const std::string &user, uid_t uid, gid_t gid)
	    -> std::optional<PreparedPaths> {
		return prepare_runtime_auth_files_from(user, uid, gid, runtime_root(),
		                                       howdy::native::resolve_config_path(),
		                                       howdy::native::resolve_user_models_dir(), 0, 0);
	}

#ifdef HOWDY_AUTH_HELPER_TESTING
	auto prepare_runtime_auth_files_for_test(const std::string &user, uid_t uid, gid_t gid,
	                                         const std::filesystem::path &runtime_root,
	                                         const std::filesystem::path &source_config,
	                                         const std::filesystem::path &source_user_models_dir,
	                                         uid_t owner_uid) -> std::optional<PreparedPaths> {
		return prepare_runtime_auth_files_from(user, uid, gid, runtime_root, source_config,
		                                       source_user_models_dir, owner_uid, gid);
	}
#endif

}  // namespace howdy::native::auth_helper
