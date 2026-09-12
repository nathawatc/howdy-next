#include "config/config_utils.hpp"
#include "config/runtime_paths.hpp"
#include "paths.hpp"
#include "test_support.hpp"

#include <filesystem>
#include <optional>
#include <unistd.h>

#include <sys/stat.h>

namespace {

	using howdy::test::expect;
	using howdy::test::write_file;

	void ClearRuntimeEnv() {
		unsetenv("HOWDY_CONFIG");
		unsetenv("HOWDY_MODELS_DIR");
		unsetenv("HOWDY_USER_MODELS_DIR");
		unsetenv("HOWDY_LOG_PATH");
	}

}  // namespace

auto main() -> int {
	namespace fs = std::filesystem;
	bool ok      = true;

	const auto      temp_root = fs::temp_directory_path() / "howdy-runtime-paths-test";
	std::error_code ec;
	fs::remove_all(temp_root, ec);
	fs::create_directories(temp_root, ec);
	ok &= expect(!ec, "create temp root");

	const auto config_path     = temp_root / "custom-config.ini";
	const auto models_dir      = temp_root / "custom-models";
	const auto user_models_dir = temp_root / "custom-user-models";
	const auto log_path        = temp_root / "custom-log";

	ClearRuntimeEnv();
	const fs::path default_config_path     = kConfiguredConfigPath;
	const fs::path default_models_dir      = kConfiguredModelsDir;
	const fs::path default_user_models_dir = kConfiguredUserModelsDir;
	const fs::path default_log_path        = kConfiguredLogPath;

	ok &= expect(howdy::native::ResolveConfigPath() == default_config_path,
	             "default config path uses configured path");
	ok &= expect(howdy::native::ResolveModelsDir() == default_models_dir,
	             "default models directory uses configured path");
	ok &= expect(howdy::native::ResolveUserModelsDir() == default_user_models_dir,
	             "default user models directory uses configured path");
	ok &= expect(howdy::native::ResolveLogPath() == default_log_path,
	             "default log path uses configured path");

	setenv("HOWDY_CONFIG", config_path.c_str(), 1);
	setenv("HOWDY_MODELS_DIR", models_dir.c_str(), 1);
	setenv("HOWDY_USER_MODELS_DIR", user_models_dir.c_str(), 1);
	setenv("HOWDY_LOG_PATH", log_path.c_str(), 1);

	ok &= expect(howdy::native::ResolveConfigPath() == config_path,
	             "resolve_config_path respects HOWDY_CONFIG");
	ok &= expect(howdy::native::ResolveModelsDir() == models_dir,
	             "resolve_models_dir respects HOWDY_MODELS_DIR");
	ok &= expect(howdy::native::ResolveUserModelsDir() == user_models_dir,
	             "resolve_user_models_dir respects HOWDY_USER_MODELS_DIR");
	ok &= expect(howdy::native::ResolveLogPath() == log_path,
	             "resolve_log_path respects HOWDY_LOG_PATH");

	setenv("HOWDY_CONFIG", "", 1);
	setenv("HOWDY_MODELS_DIR", "", 1);
	setenv("HOWDY_USER_MODELS_DIR", "", 1);
	setenv("HOWDY_LOG_PATH", "", 1);
	ok &= expect(howdy::native::ResolveConfigPath() == default_config_path,
	             "empty HOWDY_CONFIG is ignored");
	ok &= expect(howdy::native::ResolveModelsDir() == default_models_dir,
	             "empty HOWDY_MODELS_DIR is ignored");
	ok &= expect(howdy::native::ResolveUserModelsDir() == default_user_models_dir,
	             "empty HOWDY_USER_MODELS_DIR is ignored");
	ok &= expect(howdy::native::ResolveLogPath() == default_log_path,
	             "empty HOWDY_LOG_PATH is ignored");

	setenv("HOWDY_CONFIG", "relative-config.ini", 1);
	setenv("HOWDY_MODELS_DIR", "relative-models", 1);
	setenv("HOWDY_USER_MODELS_DIR", "relative-user-models", 1);
	setenv("HOWDY_LOG_PATH", "relative-log", 1);
	ok &= expect(howdy::native::ResolveConfigPath() == default_config_path,
	             "relative HOWDY_CONFIG is ignored and default config path is used");
	ok &= expect(howdy::native::ResolveModelsDir() == default_models_dir,
	             "relative HOWDY_MODELS_DIR is ignored and default models directory is used");
	ok &= expect(
	    howdy::native::ResolveUserModelsDir() == default_user_models_dir,
	    "relative HOWDY_USER_MODELS_DIR is ignored and default user models directory is used");
	ok &= expect(howdy::native::ResolveLogPath() == default_log_path,
	             "relative HOWDY_LOG_PATH is ignored and default log path is used");

	const auto insecure_dir    = temp_root / "insecure-config-dir";
	const auto insecure_config = insecure_dir / "config.ini";
	fs::create_directories(insecure_dir, ec);
	ok &= expect(!ec, "create insecure config dir");
	ok &= expect(write_file(insecure_config, "[core]\ndisabled = false\n"),
	             "write insecure config fixture");
	ok &= expect(chmod(insecure_dir.c_str(), 0777) == 0, "make config dir insecure");
	setenv("HOWDY_CONFIG", insecure_config.c_str(), 1);
	ok &= expect(howdy::native::ResolveConfigPath() == insecure_config,
	             "absolute config override still resolves before trust check");
	ok &=
	    expect(!howdy::native::CheckSecureConfigPath(insecure_config, std::nullopt, {temp_root}).ok,
	           "insecure config path is rejected before runtime use");
	ok &= expect(chmod(insecure_dir.c_str(), 0755) == 0, "restore insecure config dir mode");
	ok &=
	    expect(howdy::native::CheckSecureConfigPath(insecure_config, std::nullopt, {temp_root}).ok,
	           "restored config fixture is secure inside boundary");

	const auto insecure_models = temp_root / "insecure-models";
	fs::create_directories(insecure_models, ec);
	ok &= expect(!ec, "create insecure models dir");
	ok &= expect(chmod(insecure_models.c_str(), 0777) == 0, "make models dir insecure");
	setenv("HOWDY_MODELS_DIR", insecure_models.c_str(), 1);
	ok &= expect(howdy::native::ResolveModelsDir() == insecure_models,
	             "absolute models override still resolves before trust check");
	ok &= expect(
	    !howdy::native::CheckSecureRootOwnedDirectoryTree(
	         howdy::native::ResolveModelsDir(), "Models directory", std::nullopt, {temp_root})
	         .ok,
	    "insecure model directory is rejected before model load");
	ok &= expect(chmod(insecure_models.c_str(), 0755) == 0, "restore insecure models dir mode");
	ok &= expect(howdy::native::CheckSecureRootOwnedDirectoryTree(
	                 insecure_models, "Models directory", std::nullopt, {temp_root})
	                 .ok,
	             "restored model fixture is secure inside boundary");

	ClearRuntimeEnv();

	fs::remove_all(temp_root, ec);
	if (!ok) {
		return 1;
	}
	return 0;
}
