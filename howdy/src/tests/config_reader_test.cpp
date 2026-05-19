#include "config/config_reader.hpp"
#include "common/capture_device_path.hpp"
#include "config/config_validation.hpp"
#include "config/config_values.hpp"

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
  ok &= expect(!howdy::native::validate_runtime_config(valid).has_value(),
               "valid config passes semantic validation");
  ok &= expect(valid.parse_error() == 0, "parse_error is zero for valid config");
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
  ok &= expect(howdy::native::config_timeout_seconds(valid) == 7,
               "validated timeout keeps configured value");
  ok &= expect(howdy::native::config_dark_threshold(valid) > 55.4F &&
                   howdy::native::config_dark_threshold(valid) < 55.6F,
               "validated dark threshold keeps configured value");

  const auto missing_path = temp_root / "does-not-exist.ini";
  howdy::native::ConfigReader missing(missing_path.string());
  ok &= expect(!missing.ok(), "missing config should fail parse");
  ok &= expect(missing.parse_error() != 0,
               "parse_error is non-zero for missing file");

  const auto invalid_path = temp_root / "invalid.ini";
  ok &= expect(write_file(invalid_path,
                          "[face]\n"
                          "sface_metric = weird\n"
                          "sface_threshold = 9\n"
                          "yunet_score_threshold = 4\n"
                          "[video]\n"
                          "timeout = 0\n"
                          "dark_threshold = 1000\n"
                          "frame_width = 4\n"
                          "device_fps = 9999\n"),
               "write invalid bounded config");
  howdy::native::ConfigReader invalid(invalid_path.string());
  ok &= expect(invalid.ok(), "invalid bounded config still parses");
  ok &= expect(howdy::native::validate_runtime_config(invalid).has_value(),
               "invalid bounded config fails semantic validation");
  ok &= expect(howdy::native::config_timeout_seconds(invalid) == 4,
               "invalid timeout falls back");
  ok &= expect(howdy::native::config_dark_threshold(invalid) == 60.0F,
               "invalid dark threshold falls back");
  ok &= expect(howdy::native::config_frame_width(invalid) == -1,
               "invalid frame width falls back");
  ok &= expect(howdy::native::config_device_fps(invalid) == 0,
               "invalid device fps falls back");
  ok &= expect(howdy::native::config_sface_metric(invalid) == "cosine",
               "invalid sface metric falls back");
  ok &= expect(howdy::native::config_sface_threshold(invalid, "cosine") == 0.363F,
               "invalid sface threshold falls back");
  ok &= expect(howdy::native::is_allowed_capture_device_path("/dev/video0"),
               "video device path prefix is allowed");
  ok &= expect(howdy::native::is_allowed_capture_device_path("none"),
               "none device path remains allowed");
  ok &= expect(!howdy::native::is_allowed_capture_device_path("/tmp/camera"),
               "non-device path prefix is rejected");
  ok &= expect(!howdy::native::is_allowed_capture_device_path("/dev/null"),
               "wrong character device path is rejected");

  const auto relative_model_path = temp_root / "relative-model.ini";
  ok &= expect(write_file(relative_model_path,
                          "[face]\n"
                          "yunet_model = relative.onnx\n"),
               "write relative model path ini");
  howdy::native::ConfigReader relative_model(relative_model_path.string());
  ok &= expect(relative_model.ok(), "relative model path config should parse");
  ok &= expect(howdy::native::validate_runtime_config(relative_model).has_value(),
               "relative model path fails semantic validation");

  const auto malformed_path = temp_root / "malformed.ini";
  ok &= expect(write_file(malformed_path,
                          "[video]\n"
                          "timeout = abc\n"
                          "dark_threshold = nope\n"
                          "[face]\n"
                          "sface_threshold = bad\n"),
               "write malformed ini");
  howdy::native::ConfigReader malformed(malformed_path.string());
  ok &= expect(malformed.ok(), "malformed config should still parse");
  ok &= expect(howdy::native::validate_runtime_config(malformed).has_value(),
               "malformed numeric config fails semantic validation");
  ok &= expect(howdy::native::config_timeout_seconds(malformed) == 4,
               "malformed timeout falls back");
  ok &= expect(howdy::native::config_dark_threshold(malformed) == 60.0F,
               "malformed dark threshold falls back");
  ok &= expect(howdy::native::config_sface_threshold(malformed, "cosine") == 0.363F,
               "malformed sface threshold falls back");

  const auto non_finite_path = temp_root / "non-finite.ini";
  ok &= expect(write_file(non_finite_path,
                          "[video]\n"
                          "dark_threshold = nan\n"
                          "[face]\n"
                          "yunet_score_threshold = +inf\n"
                          "sface_threshold = -inf\n"),
               "write non-finite numeric config");
  howdy::native::ConfigReader non_finite(non_finite_path.string());
  ok &= expect(non_finite.ok(), "non-finite config should still parse");
  ok &= expect(howdy::native::validate_runtime_config(non_finite).has_value(),
               "non-finite numeric values fail semantic validation");

  const auto negative_fps_path = temp_root / "negative-fps.ini";
  ok &= expect(write_file(negative_fps_path,
                          "[video]\n"
                          "device_fps = -1\n"),
               "write negative fps config");
  howdy::native::ConfigReader negative_fps(negative_fps_path.string());
  ok &= expect(negative_fps.ok(), "negative fps config should parse");
  const auto negative_fps_validation =
      howdy::native::validate_runtime_config(negative_fps);
  ok &= expect(negative_fps_validation.has_value(),
               "negative fps fails semantic validation");
  if (negative_fps_validation.has_value()) {
    ok &= expect(negative_fps_validation->find("device_fps") != std::string::npos,
                 "negative fps validation reports key name");
    ok &= expect(negative_fps_validation->find("-1") != std::string::npos,
                 "negative fps validation reports offending value");
  }
  ok &= expect(howdy::native::config_device_fps(negative_fps) == 0,
               "negative fps still falls back to runtime default");

  fs::remove_all(temp_root, ec);
  if (!ok) {
    return 1;
  }
  return 0;
}
