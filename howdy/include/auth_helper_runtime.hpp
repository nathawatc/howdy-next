#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <unistd.h>

#include <sys/types.h>

namespace howdy::native::auth_helper {

	struct PreparedPaths {
		std::filesystem::path runtime_dir;
		std::filesystem::path config_path;
		std::filesystem::path user_models_dir;
	};

	auto runtime_root() -> std::filesystem::path;
	auto prepare_runtime_auth_files(const std::string &user, uid_t uid, gid_t gid)
	    -> std::optional<PreparedPaths>;

#ifdef HOWDY_AUTH_HELPER_TESTING
	auto validate_runtime_root(const std::filesystem::path &path) -> bool;
	auto secure_source_file_stat(int fd, const std::string &label) -> bool;
	auto write_all(int fd, const char *data, ssize_t size) -> bool;
	auto copy_file_for_user(const std::filesystem::path &source,
	                        const std::filesystem::path &destination, const std::string &label,
	                        gid_t gid) -> bool;
	auto select_source_model_path(const std::filesystem::path &source_user_models_dir,
	                              const std::string &user, std::optional<uid_t> owner_uid,
	                              std::optional<std::filesystem::path> &source_model_path) -> bool;
	auto prepare_runtime_auth_files_for_test(const std::string &user, uid_t uid, gid_t gid,
	                                         const std::filesystem::path &runtime_root,
	                                         const std::filesystem::path &source_config,
	                                         const std::filesystem::path &source_user_models_dir,
	                                         uid_t owner_uid) -> std::optional<PreparedPaths>;
#endif

}  // namespace howdy::native::auth_helper
