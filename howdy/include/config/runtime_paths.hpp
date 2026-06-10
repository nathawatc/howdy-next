#pragma once

#include <filesystem>

namespace howdy::native {

	auto resolve_config_path() -> std::filesystem::path;
	auto resolve_models_dir() -> std::filesystem::path;
	auto resolve_user_models_dir() -> std::filesystem::path;
	auto resolve_log_path() -> std::filesystem::path;

}  // namespace howdy::native
