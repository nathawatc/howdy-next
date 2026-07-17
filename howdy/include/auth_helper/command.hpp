#pragma once

#include <filesystem>
#include <string>

namespace howdy::native::auth_helper::command {

	__attribute__((visibility("hidden"))) auto
	print_prepared_paths(const std::filesystem::path &config_path,
	                     const std::filesystem::path &user_models_dir) -> void;
	__attribute__((visibility("hidden"))) auto prepare_for_user(const std::string &user) -> int;
	__attribute__((visibility("hidden"))) auto cleanup_for_user(const std::filesystem::path &path)
	    -> int;
	__attribute__((visibility("hidden"))) auto run(int argc, char **argv) -> int;

}  // namespace howdy::native::auth_helper::command
