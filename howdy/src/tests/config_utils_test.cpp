#include "config/config_utils.hpp"
#include "config/config_validation.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

auto write_file(const std::filesystem::path &path, const std::string &content)
    -> bool {
  std::ofstream out(path);
  if (!out.is_open()) {
    return false;
  }
  out << content;
  return out.good();
}

auto read_file(const std::filesystem::path &path) -> std::string {
  std::ifstream in(path);
  if (!in.is_open()) {
    return {};
  }
  return {std::istreambuf_iterator<char>(in),
          std::istreambuf_iterator<char>()};
}

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
  const auto temp_root =
      fs::temp_directory_path() / "howdy-config-utils-test-work";
  std::error_code ec;
  fs::remove_all(temp_root, ec);
  fs::create_directories(temp_root, ec);
  ok &= expect(!ec, "create temp root");

  const auto config_path = temp_root / "config.ini";
  ok &= expect(write_file(config_path,
                          "[core]\n"
                          "disabled = false\n"
                          "\n"
                          "[video]\n"
                          "dark_threshold = 60\n"),
               "write initial config");

  const auto lines = howdy::native::read_config_lines(config_path, false);
  ok &= expect(lines.size() == 5, "read_config_lines returns expected line count");
  ok &= expect(!lines.empty() && lines[0] == "[core]\n",
               "read_config_lines preserves newlines");

  ok &= expect(howdy::native::update_config_value(config_path, "disabled", "true"),
               "update_config_value succeeds for existing key");
  const auto after_disabled = read_file(config_path);
  ok &= expect(after_disabled.find("disabled = true\n") != std::string::npos,
               "disabled key updated");

  ok &= expect(howdy::native::is_safe_ini_scalar_value("true"),
               "is_safe_ini_scalar_value accepts simple scalar");
  ok &= expect(!howdy::native::is_safe_ini_scalar_value("true\n[video]\ntimeout = 0"),
               "is_safe_ini_scalar_value rejects newline injection");
  ok &= expect(!howdy::native::is_safe_ini_scalar_value("[video]"),
               "is_safe_ini_scalar_value rejects section-like values");

  ok &= expect(
      howdy::native::update_config_value(config_path, "dark_threshold", "42"),
      "update_config_value succeeds in later section");
  const auto after_threshold = read_file(config_path);
  ok &= expect(after_threshold.find("dark_threshold = 42\n") != std::string::npos,
               "dark_threshold key updated");

  ok &= expect(!howdy::native::update_config_value(config_path, "missing_key", "x"),
               "update_config_value fails for missing key");
  ok &= expect(!howdy::native::update_config_value(
                   config_path, "dark_threshold", "0\n[core]\ndisabled = true"),
               "update_config_value rejects newline injection");
  ok &= expect(read_file(config_path) == after_threshold,
               "rejected injection leaves config unchanged");
  ok &= expect(
      !howdy::native::update_config_value(config_path, "dark_threshold", "1000"),
      "update_config_value rejects semantically invalid values");
  ok &= expect(read_file(config_path) == after_threshold,
               "semantic validation failure leaves config unchanged");

  const auto nested_path = temp_root / "nested" / "generated.ini";
  const std::vector<std::string> write_lines = {
      "[face]\n",
      "sface_threshold = 0.363\n",
  };
  ok &= expect(howdy::native::atomic_write_lines(nested_path, write_lines),
               "atomic_write_lines creates parent dirs and writes file");
  ok &= expect(read_file(nested_path) == "[face]\nsface_threshold = 0.363\n",
               "atomic_write_lines output matches expected content");

  ok &= expect(chmod(config_path.c_str(), 0666) == 0,
               "make config file world-writable");
  ok &= expect(!howdy::native::update_config_value(config_path, "disabled", "false"),
               "update_config_value rejects insecure config permissions");
  ok &= expect(chmod(config_path.c_str(), 0644) == 0,
               "restore config permissions");

  const auto protected_path = temp_root / "protected.ini";
  ok &= expect(write_file(protected_path, "[core]\ndisabled = false\n"),
               "write protected config");
  ok &= expect(chmod(protected_path.c_str(), 0600) == 0,
               "set protected config mode");
  ok &= expect(howdy::native::update_config_value(protected_path, "disabled", "true"),
               "update_config_value succeeds on secure config");
  struct stat protected_stat {};
  ok &= expect(stat(protected_path.c_str(), &protected_stat) == 0,
               "stat protected config");
  ok &= expect((protected_stat.st_mode & 0777) == 0600,
               "atomic write preserves config mode");

  const auto hardlink_path = temp_root / "protected-hardlink.ini";
  ok &= expect(link(protected_path.c_str(), hardlink_path.c_str()) == 0,
               "create hard link to protected config");
  ok &= expect(!howdy::native::update_config_value(hardlink_path, "disabled", "false"),
               "update_config_value rejects hard-linked config");

  howdy::native::ConfigReader validated(config_path.string());
  ok &= expect(validated.ok(), "validated config still parses");
  ok &= expect(!howdy::native::validate_runtime_config(validated).has_value(),
               "validated config passes semantic validation");

  fs::remove_all(temp_root, ec);
  if (!ok) {
    return 1;
  }
  return 0;
}
