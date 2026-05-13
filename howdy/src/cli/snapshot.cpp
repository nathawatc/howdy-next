#include "cli/snapshot_cli.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "config/config_reader.hpp"
#include "config/runtime_paths.hpp"
#include "recorders/video_capture.hpp"

namespace {

constexpr int kExitOk = 0;
constexpr int kExitAbort = 1;

auto snapshot_path() -> std::filesystem::path {
  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  std::tm buffer {};
  gmtime_r(&time, &buffer);
  char filename[32] = {0};
  std::strftime(filename, sizeof(filename), "%Y%m%dT%H%M%S.jpg", &buffer);
  return howdy::native::resolve_log_path() / "snapshots" / filename;
}

auto generate_snapshot(const std::vector<cv::Mat> &frames,
                       const std::vector<std::string> &text_lines)
    -> std::filesystem::path {
  const int frame_height = frames.front().rows;
  cv::Mat snap;
  cv::hconcat(frames, snap);
  cv::Mat padded;
  cv::copyMakeBorder(snap, padded, 0,
                     static_cast<int>(text_lines.size()) * 20 + 40, 0, 0,
                     cv::BORDER_CONSTANT, cv::Scalar(44, 44, 44));
  snap = padded;

  for (std::size_t index = 0; index < text_lines.size(); ++index) {
    const int padding_top = frame_height + 30 + static_cast<int>(index) * 20;
    cv::putText(snap, text_lines[index], cv::Point(30, padding_top),
                cv::FONT_HERSHEY_SIMPLEX, 0.4, cv::Scalar(255, 255, 255), 0,
                cv::LINE_AA);
  }

  const auto filepath = snapshot_path();
  std::filesystem::create_directories(filepath.parent_path());
  cv::imwrite(filepath.string(), snap);
  return filepath;
}

}  // namespace

int snapshot_main(int, char **) {
  const auto config_path = howdy::native::resolve_config_path();
  howdy::native::ConfigReader config(config_path.string());
  if (!config.ok()) {
    std::cerr << "Failed to parse config: " << config_path << "\n";
    return kExitAbort;
  }

  howdy::native::VideoCapture capture(howdy::native::load_capture_settings(config));
  if (!capture.open()) {
    std::cerr << capture.error_message() << "\n";
    return kExitAbort;
  }

  std::vector<cv::Mat> frames;
  frames.reserve(4);
  while (frames.size() < 4) {
    cv::Mat frame;
    if (!capture.read(frame)) {
      capture.release();
      std::cerr << "Failed to read frame from camera\n";
      return kExitAbort;
    }
    frames.push_back(frame);
  }
  capture.release();

  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  std::tm buffer {};
  gmtime_r(&time, &buffer);
  char timestr[64] = {0};
  std::strftime(timestr, sizeof(timestr), "%Y/%m/%d %H:%M:%S UTC", &buffer);

  const auto filepath = generate_snapshot(
      frames,
      {
          "GENERATED SNAPSHOT",
          std::string("Date: ") + timestr,
          "Dark threshold config: " +
              std::to_string(config.get_float("video", "dark_threshold", 60.0F)),
          "SFace threshold config: " +
              std::to_string(config.get_float("face", "sface_threshold", 0.363F)),
      });

  std::cout << "Generated snapshot saved as\n";
  std::cout << filepath.string() << "\n";
  return kExitOk;
}
