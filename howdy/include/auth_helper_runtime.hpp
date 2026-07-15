#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <unistd.h>

#include <sys/types.h>

#ifdef HOWDY_AUTH_HELPER_TESTING
#	include <acl/libacl.h>
#endif

namespace howdy::native::auth_helper {

	struct PreparedPaths {
		std::filesystem::path runtime_dir;
		std::filesystem::path config_path;
		std::filesystem::path user_models_dir;
	};

	struct CleanupRuntimeResult {
		bool        ok = false;
		std::string error_message;
	};

	struct RuntimeIdentity {
		uid_t uid = 0;
		gid_t gid = 0;
	};

	auto runtime_root() -> std::filesystem::path;
	auto prepare_runtime_auth_files(const std::string &user, RuntimeIdentity identity)
	    -> std::optional<PreparedPaths>;
	auto cleanup_runtime_auth_files(const std::filesystem::path &path, RuntimeIdentity identity)
	    -> CleanupRuntimeResult;

#ifdef HOWDY_AUTH_HELPER_TESTING
	using AclSetFdForTest = auto (*)(int, acl_t) -> int;
	using AclGetFdForTest = auto (*)(int) -> acl_t;
	using AclResetForTest = auto (*)() -> void;

	auto validate_runtime_root(const std::filesystem::path &path) -> bool;
	auto secure_source_file_stat(int fd, const std::string &label) -> bool;
	auto write_all(int fd, const char *data, ssize_t size) -> bool;
	auto copy_file_for_user(const std::filesystem::path &source,
	                        const std::filesystem::path &destination, const std::string &label,
	                        gid_t gid) -> bool;
	auto select_source_model_path(const std::filesystem::path &source_user_models_dir,
	                              const std::string &user, std::optional<uid_t> owner_uid,
	                              std::optional<std::filesystem::path> &source_model_path) -> bool;
	auto cleanup_runtime_auth_files_for_test(const std::filesystem::path &path,
	                                         RuntimeIdentity              identity,
	                                         const std::filesystem::path &runtime_root)
	    -> CleanupRuntimeResult;

	struct RuntimeAuthTestPaths {
		std::filesystem::path runtime_root;
		std::filesystem::path source_config;
		std::filesystem::path source_user_models_dir;
	};

	auto prepare_runtime_auth_files_for_test(const std::string &user, RuntimeIdentity identity,
	                                         RuntimeAuthTestPaths paths, uid_t owner_uid)
	    -> std::optional<PreparedPaths>;
	auto set_acl_setup_failure_for_test(bool fail) -> void;
	auto set_acl_verification_failure_for_test(bool fail) -> void;
	auto set_acl_io_for_test(AclSetFdForTest set_fd, AclGetFdForTest get_fd, AclResetForTest reset)
	    -> void;
	auto reset_acl_io_for_test() -> void;
#endif

}  // namespace howdy::native::auth_helper
