#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#include "common/compare_exit.hpp"
#include "config/config_reader.hpp"
#include "paths.hpp"
#include "recorders/video_capture.hpp"

namespace {

using howdy::native::CompareExit;

struct Args {
  std::string config_path;
  int frames = 1;
};

void print_usage(const char *argv0) {
  std::cout << "Usage: " << argv0 << " [--config PATH] [--frames N]\n";
  std::cout << "Open the configured camera through the native C++ recorder and "
               "read a few frames.\n";
}

auto resolve_default_config() -> std::string {
  if (const char *env_config = std::getenv("HOWDY_CONFIG")) {
    return env_config;
  }

  if (std::filesystem::exists(kDefaultDevConfigPath)) {
    return kDefaultDevConfigPath;
  }

  return kInstalledConfigPath;
}

auto parse_args(int argc, char *argv[]) -> Args {
  Args args{.config_path = resolve_default_config()};

  for (int index = 1; index < argc; ++index) {
    const std::string_view arg(argv[index]);
    if (arg == "--help" || arg == "-h") {
      print_usage(argv[0]);
      std::exit(static_cast<int>(CompareExit::kSuccess));
    }
    if (arg == "--config" && index + 1 < argc) {
      args.config_path = argv[++index];
      continue;
    }
    if (arg == "--frames" && index + 1 < argc) {
      args.frames = std::max(1, std::atoi(argv[++index]));
      continue;
    }

    std::cerr << "Unknown argument: " << arg << "\n";
    print_usage(argv[0]);
    std::exit(static_cast<int>(CompareExit::kAbort));
  }

  return args;
}

}  // namespace

auto main(int argc, char *argv[]) -> int {
  const Args args = parse_args(argc, argv);

  howdy::native::ConfigReader config(args.config_path);
  if (!config.ok()) {
    std::cerr << "Failed to parse config: " << args.config_path
              << " (error " << config.parse_error() << ")\n";
    return static_cast<int>(CompareExit::kAbort);
  }

  howdy::native::VideoCapture capture(
      howdy::native::load_capture_settings(config));
  if (!capture.open()) {
    std::cerr << capture.error_message() << "\n";
    if (capture.error() == howdy::native::CaptureError::kMissingDevice ||
        capture.error() == howdy::native::CaptureError::kOpenFailed) {
      return static_cast<int>(CompareExit::kInvalidDevice);
    }
    return static_cast<int>(CompareExit::kAbort);
  }

  cv::Mat frame;
  cv::Mat gray_frame;
  for (int frame_index = 0; frame_index < args.frames; ++frame_index) {
    if (!capture.read(frame, &gray_frame)) {
      std::cerr << capture.error_message() << "\n";
      return static_cast<int>(CompareExit::kAbort);
    }
  }

  std::cout << "frame=" << frame.cols << "x" << frame.rows
            << " gray_channels=" << gray_frame.channels() << "\n";
  return static_cast<int>(CompareExit::kSuccess);
}
