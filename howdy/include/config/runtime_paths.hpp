#pragma once

#include <filesystem>

namespace howdy::native {

	auto ResolveConfigPath() -> std::filesystem::path;
	auto ResolveModelsDir() -> std::filesystem::path;
	auto ResolveUserModelsDir() -> std::filesystem::path;
	auto ResolveLogPath() -> std::filesystem::path;

}  // namespace howdy::native
