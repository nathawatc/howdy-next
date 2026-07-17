#include "auth_helper/command.hpp"

#include "auth_helper/runtime.hpp"
#include "protocol/auth_helper_protocol.hpp"
#include "support/user_names.hpp"

#include <filesystem>
#include <iostream>
#include <pwd.h>
#include <string>
#include <unistd.h>

namespace howdy::native::auth_helper::command {

	namespace {

		auto usage(const char *argv0) -> void {
			std::cout << "Usage: " << argv0 << " prepare <user>\n"
			          << "       " << argv0 << " cleanup <runtime-dir>\n";
		}

		auto fail(const std::string &message) -> int {
			std::cerr << message << "\n";
			return 1;
		}

	}  // namespace

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

	auto run(int argc, char **argv) -> int {
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

}  // namespace howdy::native::auth_helper::command
