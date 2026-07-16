#pragma once

#include "runtime/runtime_session.hpp"

#include <filesystem>
#include <string_view>

namespace howdy::pam::auth_helper_process {

	__attribute__((visibility("hidden"))) auto
	prepare_runtime_auth_files(std::string_view username, PreparedRuntimeFiles *prepared) -> bool;
	__attribute__((visibility("hidden"))) auto
	cleanup_runtime_auth_files(const std::filesystem::path &root_dir) -> void;

}  // namespace howdy::pam::auth_helper_process
