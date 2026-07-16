#include "auth_helper/runtime.hpp"
#include "protocol/auth_helper_protocol.hpp"
#include "support/user_names.hpp"
#ifdef HOWDY_AUTH_HELPER_TESTING
#	include "support/auth_helper_testing.hpp"
#endif

#include <filesystem>
#include <iostream>
#include <pwd.h>
#include <string>
#include <unistd.h>

namespace {

#ifndef HOWDY_AUTH_HELPER_TESTING
	auto usage(const char *argv0) -> void {
		std::cout << "Usage: " << argv0 << " prepare <user>\n"
		          << "       " << argv0 << " cleanup <runtime-dir>\n";
	}
#endif

	auto fail(const std::string &message) -> int {
		std::cerr << message << "\n";
		return 1;
	}

	auto print_prepared_paths(const std::filesystem::path &config_path,
	                          const std::filesystem::path &user_models_dir) -> void {
		using namespace howdy::native::auth_helper_protocol;

		std::cout << kConfigPathKey << "=" << config_path.string() << "\n";
		std::cout << kUserModelsDirKey << "=" << user_models_dir.string() << "\n";
	}

	auto prepare_for_user(const std::string &user) -> int {
		if (geteuid() != 0) {
			return fail("howdy-auth-helper must be installed setuid root");
		}

		if (!howdy::native::is_valid_model_user_name(user)) {
			return fail(howdy::native::kInvalidUserNameMessage);
		}

		const uid_t   uid   = getuid();
		const passwd *entry = getpwuid(uid);
		if (entry == nullptr) {
			return fail("Failed to resolve calling user");
		}
		if (entry->pw_name == nullptr || user != entry->pw_name) {
			return fail("howdy-auth-helper can only prepare auth files for the calling user");
		}

		const auto prepared = howdy::native::auth_helper::prepare_runtime_auth_files(
		    user, {.uid = uid, .gid = entry->pw_gid});
		if (!prepared.has_value()) {
			return 1;
		}

		print_prepared_paths(prepared->config_path, prepared->user_models_dir);
		return 0;
	}

	auto cleanup_for_user(const std::filesystem::path &path) -> int {
		if (geteuid() != 0) {
			return fail("howdy-auth-helper must be installed setuid root");
		}

		const uid_t   uid   = getuid();
		const passwd *entry = getpwuid(uid);
		if (entry == nullptr) {
			return fail("Failed to resolve calling user");
		}

		const auto cleanup_result = howdy::native::auth_helper::cleanup_runtime_auth_files(
		    path, {.uid = uid, .gid = entry->pw_gid});
		if (!cleanup_result.ok) {
			return fail(cleanup_result.error_message);
		}
		return 0;
	}

}  // namespace

#ifdef HOWDY_AUTH_HELPER_TESTING
namespace howdy::native::testing {

	auto runtime_root() -> std::filesystem::path {
		return auth_helper::runtime_root();
	}

	auto validate_runtime_root(const std::filesystem::path &path) -> bool {
		return auth_helper::validate_runtime_root(path);
	}

	auto secure_source_file_stat(int fd, const std::string &label) -> bool {
		return auth_helper::secure_source_file_stat(fd, label);
	}

	auto write_all(int fd, const char *data, ssize_t size) -> bool {
		return auth_helper::write_all(fd, data, size);
	}

	auto copy_file_for_user(const std::filesystem::path &source,
	                        const std::filesystem::path &destination, const std::string &label,
	                        gid_t gid) -> bool {
		return auth_helper::copy_file_for_user(source, destination, label, gid);
	}

	auto select_source_model_path(const std::filesystem::path &source_user_models_dir,
	                              const std::string &user, std::optional<uid_t> owner_uid,
	                              std::optional<std::filesystem::path> &source_model_path) -> bool {
		return auth_helper::select_source_model_path(source_user_models_dir, user, owner_uid,
		                                             source_model_path);
	}

	auto print_prepared_paths(const std::filesystem::path &config_path,
	                          const std::filesystem::path &user_models_dir) -> void {
		::print_prepared_paths(config_path, user_models_dir);
	}

	auto prepare_for_user(const std::string &user) -> int {
		return ::prepare_for_user(user);
	}

	auto cleanup_for_user(const std::filesystem::path &path) -> int {
		return ::cleanup_for_user(path);
	}

}  // namespace howdy::native::testing
#else
auto main(int argc, char **argv) -> int {
	if (argc == 2 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) {
		usage(argv[0]);
		return 0;
	}

	if (argc == 3 && std::string(argv[1]) == "cleanup") {
		return cleanup_for_user(argv[2]);
	}

	if (argc != 3 || std::string(argv[1]) != "prepare") {
		usage(argv[0]);
		return 1;
	}

	return prepare_for_user(argv[2]);
}
#endif
