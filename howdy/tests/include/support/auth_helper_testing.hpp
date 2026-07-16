#ifndef HOWDY_AUTH_HELPER_TESTING_HPP
#define HOWDY_AUTH_HELPER_TESTING_HPP

#ifdef HOWDY_AUTH_HELPER_TESTING

#	include <filesystem>
#	include <optional>
#	include <string>
#	include <unistd.h>

#	include <sys/types.h>

namespace howdy::native::testing {

	auto runtime_root() -> std::filesystem::path;
	auto validate_runtime_root(const std::filesystem::path &path) -> bool;
	auto secure_source_file_stat(int fd, const std::string &label) -> bool;
	auto write_all(int fd, const char *data, ssize_t size) -> bool;
	auto copy_file_for_user(const std::filesystem::path &source,
	                        const std::filesystem::path &destination, const std::string &label,
	                        gid_t gid) -> bool;
	auto select_source_model_path(const std::filesystem::path &source_user_models_dir,
	                              const std::string &user, std::optional<uid_t> owner_uid,
	                              std::optional<std::filesystem::path> &source_model_path) -> bool;
	auto print_prepared_paths(const std::filesystem::path &config_path,
	                          const std::filesystem::path &user_models_dir) -> void;
	auto prepare_for_user(const std::string &user) -> int;
	auto cleanup_for_user(const std::filesystem::path &path) -> int;

}  // namespace howdy::native::testing

#endif

#endif  // HOWDY_AUTH_HELPER_TESTING_HPP
