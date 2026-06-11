#include "common/compare_exit.hpp"
#include "config/runtime_config.hpp"
#include "config/runtime_paths.hpp"
#include "recorders/video_capture.hpp"

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

namespace {

	using howdy::native::CompareExit;

	struct Args {
		std::string config_path;
		int         frames = 1;
	};

	void print_usage(const char *argv0) {
		std::cout << "Usage: " << argv0 << " [--config PATH] [--frames N]\n";
		std::cout << "Open the configured camera through the native C++ recorder and "
		             "read a few frames.\n";
	}

	auto resolve_default_config() -> std::string {
		return howdy::native::resolve_config_path().string();
	}

	auto parse_args(int argc, char **argv) -> Args {
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
				char *end         = nullptr;
				errno             = 0;
				const long parsed = std::strtol(argv[++index], &end, 10);
				if (end == argv[index] || *end != '\0' || errno == ERANGE) {
					std::cerr << "Invalid --frames value: " << argv[index] << "\n";
					std::exit(static_cast<int>(CompareExit::kAbort));
				}
				if (parsed > std::numeric_limits<int>::max()) {
					std::cerr << "Invalid --frames value: " << argv[index] << "\n";
					std::exit(static_cast<int>(CompareExit::kAbort));
				}
				args.frames = std::max(1, static_cast<int>(parsed));
				continue;
			}

			std::cerr << "Unknown argument: " << arg << "\n";
			print_usage(argv[0]);
			std::exit(static_cast<int>(CompareExit::kAbort));
		}

		return args;
	}

}  // namespace

auto main(int argc, char **argv) -> int {
	const Args args = parse_args(argc, argv);

	auto config_result = howdy::native::load_runtime_config(args.config_path);
	if (config_result.status != howdy::native::RuntimeConfigLoadStatus::kOk ||
	    !config_result.config.has_value()) {
		std::cerr << config_result.error_message << "\n";
		return static_cast<int>(CompareExit::kAbort);
	}
	const auto &config = *config_result.config;

	howdy::native::VideoCapture capture(howdy::native::load_capture_settings(config.video));
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
