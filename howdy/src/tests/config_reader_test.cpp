#include "config/config_reader.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

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

  const auto temp_root = fs::temp_directory_path() / "howdy-config-reader-test";
  std::error_code ec;
  fs::remove_all(temp_root, ec);
  fs::create_directories(temp_root, ec);
  ok &= expect(!ec, "create temp root");

  const auto valid_path = temp_root / "valid.ini";
  ok &= expect(write_file(valid_path,
                          "[core]\n"
                          "disabled = true\n"
                          "[video]\n"
                          "timeout = 7\n"
                          "dark_threshold = 55.5\n"),
               "write valid ini");

  howdy::native::ConfigReader valid(valid_path.string());
  ok &= expect(valid.ok(), "valid config should parse");
  ok &= expect(valid.parse_error() == 0, "parse_error is zero for valid config");
  ok &= expect(valid.path() == valid_path.string(), "path accessor returns input");
  ok &= expect(valid.get("core", "disabled", "false") == "true",
               "get string from valid config");
  ok &= expect(valid.get("core", "missing", "fallback") == "fallback",
               "fallback for missing string");
  ok &= expect(valid.get_int("video", "timeout", 3) == 7,
               "get_int returns configured value");
  ok &= expect(valid.get_int("video", "missing_timeout", 3) == 3,
               "get_int fallback for missing value");
  ok &= expect(valid.get_float("video", "dark_threshold", 1.0F) > 55.4F &&
                   valid.get_float("video", "dark_threshold", 1.0F) < 55.6F,
               "get_float returns configured value");
  ok &= expect(valid.get_bool("core", "disabled", false),
               "get_bool returns configured true");
  ok &= expect(valid.get_bool("core", "missing_bool", true),
               "get_bool fallback for missing value");

  const auto missing_path = temp_root / "does-not-exist.ini";
  howdy::native::ConfigReader missing(missing_path.string());
  ok &= expect(!missing.ok(), "missing config should fail parse");
  ok &= expect(missing.parse_error() != 0,
               "parse_error is non-zero for missing file");

  fs::remove_all(temp_root, ec);
  if (!ok) {
    return 1;
  }
  return 0;
}
