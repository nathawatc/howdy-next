#pragma once

#include <filesystem>
#include <string>

namespace howdy::native::auth_helper::command {

	__attribute__((visibility("hidden"))) auto
	PrintPreparedPaths(const std::filesystem::path &config_path,
	                   const std::filesystem::path &user_models_dir) -> void;
	__attribute__((visibility("hidden"))) auto PrepareForUser(const std::string &user) -> int;
	__attribute__((visibility("hidden"))) auto Run(int argc, char **argv) -> int;

}  // namespace howdy::native::auth_helper::command
