#include "recorders/video_capture.hpp"

#include <filesystem>
#include <utility>

#include <opencv2/imgproc.hpp>

namespace howdy::native {

namespace {

constexpr auto kNoDevice = "none";

}  // namespace

auto load_capture_settings(const ConfigReader &config) -> CaptureSettings {
  return CaptureSettings{
      .device_path = config.get("video", "device_path", "/dev/video0"),
      .warn_no_device = config.get_bool("video", "warn_no_device", true),
      .force_mjpeg = config.get_bool("video", "force_mjpeg", false),
      .frame_width = config.get_int("video", "frame_width", -1),
      .frame_height = config.get_int("video", "frame_height", -1),
      .device_fps = config.get_int("video", "device_fps", 0),
  };
}

VideoCapture::VideoCapture(CaptureSettings settings)
    : settings_(std::move(settings)) {}

auto VideoCapture::open() -> bool {
  release();
  error_ = CaptureError::kNone;
  error_message_.clear();

  if (settings_.device_path != kNoDevice &&
      !std::filesystem::exists(settings_.device_path)) {
    if (settings_.warn_no_device) {
      set_error(CaptureError::kMissingDevice,
                "Configured camera device does not exist: " +
                    settings_.device_path);
      return false;
    }
  }

  capture_.open(settings_.device_path, cv::CAP_V4L);
  if (!capture_.isOpened()) {
    set_error(CaptureError::kOpenFailed,
              "Failed to open camera device: " + settings_.device_path);
    return false;
  }

  if (settings_.device_fps > 0) {
    capture_.set(cv::CAP_PROP_FPS, settings_.device_fps);
  }
  if (settings_.force_mjpeg) {
    capture_.set(cv::CAP_PROP_FOURCC,
                 cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
  }
  if (settings_.frame_width != -1) {
    capture_.set(cv::CAP_PROP_FRAME_WIDTH, settings_.frame_width);
  }
  if (settings_.frame_height != -1) {
    capture_.set(cv::CAP_PROP_FRAME_HEIGHT, settings_.frame_height);
  }

  grab();
  return true;
}

auto VideoCapture::grab() -> bool {
  if (!capture_.isOpened()) {
    set_error(CaptureError::kOpenFailed, "Camera is not open");
    return false;
  }

  if (!capture_.grab()) {
    set_error(CaptureError::kReadFailed, "Failed to grab a frame from camera");
    return false;
  }
  return true;
}

auto VideoCapture::read(cv::Mat &frame, cv::Mat *gray_frame) -> bool {
  if (!capture_.isOpened()) {
    set_error(CaptureError::kOpenFailed, "Camera is not open");
    return false;
  }

  if (!capture_.read(frame) || frame.empty()) {
    set_error(CaptureError::kReadFailed, "Failed to read a frame from camera");
    return false;
  }

  if (gray_frame != nullptr) {
    if (frame.channels() == 3) {
      cv::cvtColor(frame, *gray_frame, cv::COLOR_BGR2GRAY);
    } else if (frame.channels() == 4) {
      cv::cvtColor(frame, *gray_frame, cv::COLOR_BGRA2GRAY);
    } else {
      *gray_frame = frame;
    }
  }

  return true;
}

void VideoCapture::release() {
  if (capture_.isOpened()) {
    capture_.release();
  }
}

auto VideoCapture::is_open() const -> bool { return capture_.isOpened(); }

auto VideoCapture::get(int property) const -> double {
  return capture_.isOpened() ? capture_.get(property) : 0.0;
}

auto VideoCapture::set(int property, double value) -> bool {
  return capture_.isOpened() && capture_.set(property, value);
}

auto VideoCapture::error() const -> CaptureError { return error_; }

auto VideoCapture::error_message() const -> const std::string & {
  return error_message_;
}

auto VideoCapture::settings() const -> const CaptureSettings & {
  return settings_;
}

void VideoCapture::set_error(CaptureError error, std::string message) {
  error_ = error;
  error_message_ = std::move(message);
}

}  // namespace howdy::native
