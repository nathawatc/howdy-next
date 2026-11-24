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

	[[nodiscard]] auto ResolveConfigPath() -> std::filesystem::path;
	[[nodiscard]] auto ResolveModelsDir() -> std::filesystem::path;
	[[nodiscard]] auto ResolveUserModelsDir() -> std::filesystem::path;
	[[nodiscard]] auto ResolveLogPath() -> std::filesystem::path;

}  // namespace howdy::native
