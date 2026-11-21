#pragma once

#include <array>
#include <filesystem>

namespace howdy::native {

	inline constexpr const char *kConfigPathEnvVar    = "HOWDY_CONFIG";
	inline constexpr const char *kModelsDirEnvVar     = "HOWDY_MODELS_DIR";
	inline constexpr const char *kUserModelsDirEnvVar = "HOWDY_USER_MODELS_DIR";
	inline constexpr const char *kLogPathEnvVar       = "HOWDY_LOG_PATH";
	inline constexpr std::array  kRuntimePathOverrideEnvVars{
	    kConfigPathEnvVar,
	    kModelsDirEnvVar,
	    kUserModelsDirEnvVar,
	    kLogPathEnvVar,
	};

	auto ResolveConfigPath() -> std::filesystem::path;
	auto ResolveModelsDir() -> std::filesystem::path;
	auto ResolveUserModelsDir() -> std::filesystem::path;
	auto ResolveLogPath() -> std::filesystem::path;

}  // namespace howdy::native
