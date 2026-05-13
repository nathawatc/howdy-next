#include "config/runtime_paths.hpp"

#include <filesystem>
#include <iostream>
#include <string>

namespace {

auto expect(bool condition, const std::string &message) -> bool {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    return false;
  }
  return true;
}

}  // namespace

auto main() -> int {
  namespace fs = std::filesystem;
  bool ok = true;

  const auto temp_root = fs::temp_directory_path() / "howdy-runtime-paths-test";
  std::error_code ec;
  fs::remove_all(temp_root, ec);
  fs::create_directories(temp_root, ec);
  ok &= expect(!ec, "create temp root");

  const auto config_path = temp_root / "custom-config.ini";
  const auto models_dir = temp_root / "custom-models";
  const auto user_models_dir = temp_root / "custom-user-models";
  const auto log_path = temp_root / "custom-log";

  setenv("HOWDY_CONFIG", config_path.c_str(), 1);
  setenv("HOWDY_MODELS_DIR", models_dir.c_str(), 1);
  setenv("HOWDY_USER_MODELS_DIR", user_models_dir.c_str(), 1);
  setenv("HOWDY_LOG_PATH", log_path.c_str(), 1);

  ok &= expect(howdy::native::resolve_config_path() == config_path,
               "resolve_config_path respects HOWDY_CONFIG");
  ok &= expect(howdy::native::resolve_models_dir() == models_dir,
               "resolve_models_dir respects HOWDY_MODELS_DIR");
  ok &= expect(howdy::native::resolve_user_models_dir() == user_models_dir,
               "resolve_user_models_dir respects HOWDY_USER_MODELS_DIR");
  ok &= expect(howdy::native::resolve_log_path() == log_path,
               "resolve_log_path respects HOWDY_LOG_PATH");

  unsetenv("HOWDY_CONFIG");
  unsetenv("HOWDY_MODELS_DIR");
  unsetenv("HOWDY_USER_MODELS_DIR");
  unsetenv("HOWDY_LOG_PATH");

  fs::remove_all(temp_root, ec);
  if (!ok) {
    return 1;
  }
  return 0;
}
