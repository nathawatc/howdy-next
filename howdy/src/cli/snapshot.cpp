#include "cli/snapshot_cli.hpp"

#include <sys/stat.h>
#include <array>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "common/atomic_files.hpp"
#include "common/file_security.hpp"
#include "config/config_reader.hpp"
#include "config/config_utils.hpp"
#include "config/config_validation.hpp"
#include "config/config_values.hpp"
#include "config/runtime_paths.hpp"
#include "recorders/video_capture.hpp"

namespace {

constexpr int kExitOk = 0;
constexpr int kExitAbort = 1;
constexpr mode_t kSnapshotDirectoryMode =
    S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IXGRP;
constexpr mode_t kSnapshotFileMode = S_IRUSR | S_IWUSR;

auto snapshot_path() -> std::filesystem::path {
  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  std::tm buffer {};
  gmtime_r(&time, &buffer);
  std::array<char, 32> filename{};
  std::strftime(filename.data(), filename.size(), "%Y%m%dT%H%M%S.jpg",
                &buffer);
  return howdy::native::resolve_log_path() / "snapshots" / filename.data();
}

auto ensure_snapshot_directory(const std::filesystem::path &directory) -> bool {
  const auto log_root = directory.parent_path();
  if (std::filesystem::exists(log_root)) {
    const auto root_security =
        howdy::native::check_secure_root_owned_directory_tree(
            log_root, "Log directory");
    if (!root_security.ok) {
      std::cerr << root_security.error_message << "\n";
      return false;
    }
  }

  std::error_code ec;
  std::filesystem::create_directories(directory, ec);
  if (ec) {
    std::cerr << "Failed to create snapshot directory: " << directory << "\n";
    return false;
  }
  if (chmod(log_root.c_str(), kSnapshotDirectoryMode) != 0 ||
      chmod(directory.c_str(), kSnapshotDirectoryMode) != 0) {
    std::cerr << "Failed to secure snapshot directory: " << directory << "\n";
    return false;
  }

  const auto root_security =
      howdy::native::check_secure_root_owned_directory_tree(
          log_root, "Log directory");
  if (!root_security.ok) {
    std::cerr << root_security.error_message << "\n";
    return false;
  }

  const auto directory_security =
      howdy::native::check_secure_root_owned_directory_tree(
          directory, "Snapshot directory");
  if (!directory_security.ok) {
    std::cerr << directory_security.error_message << "\n";
    return false;
  }
  return true;
}

auto generate_snapshot(const std::vector<cv::Mat> &frames,
                       const std::vector<std::string> &text_lines)
    -> std::filesystem::path {
  const int frame_height = frames.front().rows;
  cv::Mat snap;
  cv::hconcat(frames, snap);
  cv::Mat padded;
  cv::copyMakeBorder(snap, padded, 0,
                     (static_cast<int>(text_lines.size()) * 20) + 40, 0, 0,
                     cv::BORDER_CONSTANT, cv::Scalar(44, 44, 44));
  snap = padded;

  for (std::size_t index = 0; index < text_lines.size(); ++index) {
    const int padding_top = frame_height + 30 + (static_cast<int>(index) * 20);
    cv::putText(snap, text_lines[index], cv::Point(30, padding_top),
                cv::FONT_HERSHEY_SIMPLEX, 0.4, cv::Scalar(255, 255, 255), 0,
                cv::LINE_AA);
  }

  auto filepath = snapshot_path();
  if (!ensure_snapshot_directory(filepath.parent_path())) {
    return {};
  }
  if (!cv::imwrite(filepath.string(), snap)) {
    return {};
  }
  if (chmod(filepath.c_str(), kSnapshotFileMode) != 0) {
    std::error_code ec;
    std::filesystem::remove(filepath, ec);
    return {};
  }
  howdy::native::sync_parent_directory(filepath);
  return filepath;
}

}  // namespace

int snapshot_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  const auto config_path = howdy::native::resolve_config_path();
  const auto config_security =
      howdy::native::check_secure_config_path(config_path);
  if (!config_security.ok) {
    std::cerr << config_security.error_message << "\n";
    return kExitAbort;
  }
  howdy::native::ConfigReader config(config_path.string());
  if (!config.ok()) {
    std::cerr << "Failed to parse config: " << config_path << "\n";
    return kExitAbort;
  }
  if (const auto validation = howdy::native::validate_runtime_config(config)) {
    std::cerr << *validation << "\n";
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
  std::array<char, 64> timestr{};
  std::strftime(timestr.data(), timestr.size(), "%Y/%m/%d %H:%M:%S UTC",
                &buffer);

  const auto filepath = generate_snapshot(
      frames,
      {
          "GENERATED SNAPSHOT",
          std::string("Date: ") + timestr.data(),
          "Dark threshold config: " +
              std::to_string(howdy::native::config_dark_threshold(config)),
          "SFace threshold config: " +
              std::to_string(howdy::native::config_sface_threshold(
                  config, howdy::native::config_sface_metric(config))),
      });
  if (filepath.empty()) {
    std::cerr << "Failed to write snapshot\n";
    return kExitAbort;
  }

  std::cout << "Generated snapshot saved as\n";
  std::cout << filepath.string() << "\n";
  return kExitOk;
}
