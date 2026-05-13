#pragma once

#include <string>

#include <opencv2/videoio.hpp>

#include "config/config_reader.hpp"

namespace howdy::native {

enum class CaptureError {
  kNone,
  kMissingDevice,
  kUnsupportedPlugin,
  kOpenFailed,
  kReadFailed,
};

struct CaptureSettings {
  std::string device_path;
  std::string recording_plugin;
  std::string device_format;
  bool warn_no_device = true;
  bool force_mjpeg = false;
  int frame_width = -1;
  int frame_height = -1;
  int device_fps = 0;
};

auto load_capture_settings(const ConfigReader &config) -> CaptureSettings;

class VideoCapture {
public:
  explicit VideoCapture(CaptureSettings settings);

  auto open() -> bool;
  auto grab() -> bool;
  auto read(cv::Mat &frame, cv::Mat *gray_frame = nullptr) -> bool;
  void release();

  [[nodiscard]] auto is_open() const -> bool;
  [[nodiscard]] auto get(int property) const -> double;
  auto set(int property, double value) -> bool;

  [[nodiscard]] auto error() const -> CaptureError;
  [[nodiscard]] auto error_message() const -> const std::string &;
  [[nodiscard]] auto settings() const -> const CaptureSettings &;

private:
  void set_error(CaptureError error, std::string message);

  CaptureSettings settings_;
  cv::VideoCapture capture_;
  CaptureError error_ = CaptureError::kNone;
  std::string error_message_;
};

}  // namespace howdy::native
