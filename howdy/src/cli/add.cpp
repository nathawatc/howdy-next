#include "cli/add_cli.hpp"
#include "config/runtime_config.hpp"
#include "core/face_model.hpp"
#include "recorders/video_capture.hpp"
#include "storage/user_models.hpp"

#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <opencv2/imgproc.hpp>

namespace {

	constexpr auto kExitOk    = 0;
	constexpr auto kExitAbort = 1;
	constexpr int  kMaxFrames = 60;

	struct AddArgs {
		std::string user;
		std::string label;
		bool        plain = false;
		bool        yes   = false;
	};

	auto parse_args(int argc, char **argv) -> AddArgs {
		AddArgs args;
		if (argc < 2) {
			std::cerr << "Usage: howdy-add <user> [label] [--plain] [-y]\n";
			std::exit(kExitAbort);
		}

		args.user = argv[1];
		for (int index = 2; index < argc; ++index) {
			std::string_view arg(argv[index]);
			if (arg == "--plain") {
				args.plain = true;
				continue;
			}
			if (arg == "-y") {
				args.yes = true;
				continue;
			}
			if (args.label.empty()) {
				args.label = argv[index];
			}
		}
		return args;
	}

}  // namespace

auto add_main(int argc, char **argv) -> int {
	const auto args          = parse_args(argc, argv);
	auto       config_result = howdy::native::load_runtime_config();
	if (config_result.status != howdy::native::RuntimeConfigLoadStatus::kOk ||
	    !config_result.config.has_value()) {
		std::cerr << config_result.error_message << "\n";
		return kExitAbort;
	}
	const auto &config = *config_result.config;

	howdy::native::FaceModel face_model(config.face);
	if (!face_model.ok()) {
		std::cerr << face_model.error_message() << "\n";
		return kExitAbort;
	}

	const auto entries = howdy::native::list_user_model_entries(
	    args.user, howdy::native::FaceModel::kBackendName, face_model.metric(),
	    howdy::native::FaceModel::kSfaceModel);
	if (entries.status == howdy::native::UserModelStatus::kIncompatibleBackend ||
	    entries.status == howdy::native::UserModelStatus::kIncompatibleMetric ||
	    entries.status == howdy::native::UserModelStatus::kIncompatibleModel) {
		std::cerr << "Existing face models use incompatible face-recognition metadata.\n";
		std::cerr << "Please run `howdy clear` and enroll again with `howdy add`.\n";
		return kExitAbort;
	}
	if (entries.status != howdy::native::UserModelStatus::kOk &&
	    entries.status != howdy::native::UserModelStatus::kNoModel &&
	    entries.status != howdy::native::UserModelStatus::kNoModelDirectory) {
		std::cerr << entries.error_message << "\n";
		return kExitAbort;
	}
	std::string label = args.label;
	if (!args.yes && args.label.empty() && !args.plain) {
		std::cout << "Enter a label for this new model [automatic]: ";
		std::string input;
		std::getline(std::cin, input);
		if (!input.empty()) {
			label = input.substr(0, 24);
		}
	}
	std::erase(label, ',');

	howdy::native::VideoCapture capture(howdy::native::load_capture_settings(config.video));
	if (!capture.open()) {
		std::cerr << capture.error_message() << "\n";
		return kExitAbort;
	}

	const float        dark_threshold = config.video.dark_threshold;
	const bool         use_clahe      = config.video.clahe_enabled;
	const auto         clip_limit     = config.video.clahe_clip_limit;
	const auto         tile_size      = config.video.clahe_tile_grid_size;
	cv::Ptr<cv::CLAHE> clahe;
	if (use_clahe) {
		clahe = cv::createCLAHE(clip_limit, cv::Size(tile_size, tile_size));
	}

	if (!args.plain) {
		std::cout << "\nPlease look straight into the camera\n";
	}
	std::this_thread::sleep_for(std::chrono::seconds(2));

	cv::Mat              frame;
	cv::Mat              gray;
	std::vector<cv::Mat> faces;
	int                  valid_frames       = 0;
	int                  dark_tries         = 0;
	double               dark_running_total = 0.0;

	for (int frame_count = 0; frame_count < kMaxFrames; ++frame_count) {
		if (!capture.read(frame, &gray)) {
			continue;
		}

		if (use_clahe) {
			clahe->apply(gray, gray);
		}

		cv::Mat                        hist;
		constexpr std::array<int, 1>   hist_size{8};
		constexpr std::array<float, 2> hist_range{0.0F, 256.0F};
		std::vector<const float *>     ranges{hist_range.data()};
		constexpr std::array<int, 1>   channels{0};
		cv::calcHist(&gray, 1, channels.data(), cv::Mat(), hist, 1, hist_size.data(),
		             ranges.data());
		const double hist_total = cv::sum(hist)[0];
		if (hist_total == 0.0) {
			continue;
		}

		const auto darkness = static_cast<float>(hist.at<float>(0) / hist_total * 100.0);
		if (darkness >= 100.0F) {
			continue;
		}

		valid_frames++;
		dark_running_total += darkness;
		if (darkness > dark_threshold) {
			dark_tries++;
			continue;
		}

		auto prepared = face_model.prepare_frame(gray);
		faces         = face_model.detect(prepared);
		if (!faces.empty()) {
			frame = prepared;
			break;
		}
	}

	capture.release();

	if (faces.empty()) {
		if (valid_frames == 0) {
			std::cerr << "Camera saw only black frames - is IR emitter working?\n";
		} else if (valid_frames == dark_tries) {
			std::cerr << "All frames were too dark, please check dark_threshold in config\n";
			std::cerr << "Average darkness: " << (dark_running_total / valid_frames)
			          << ", Threshold: " << dark_threshold << "\n";
		} else {
			std::cerr << "No face detected, aborting\n";
		}
		return kExitAbort;
	}

	if (faces.size() > 1) {
		std::cerr << "Multiple faces detected, aborting\n";
		return kExitAbort;
	}

	auto encoding = face_model.encode(frame, faces.front());
	if (encoding.empty()) {
		std::cerr << "No valid face encoding captured\n";
		return kExitAbort;
	}

	const auto append_result = howdy::native::append_user_model_entry(
	    args.user, howdy::native::NewUserModelEntry{
	                   .label     = label,
	                   .backend   = howdy::native::FaceModel::kBackendName,
	                   .metric    = face_model.metric(),
	                   .model     = howdy::native::FaceModel::kSfaceModel,
	                   .encodings = {std::move(encoding)},
	               });
	if (append_result.status != howdy::native::UserModelStatus::kOk) {
		std::cerr << append_result.error_message << "\n";
		return kExitAbort;
	}

	std::cout << "\nScan complete\nAdded a new model to " << args.user << "\n";
	return kExitOk;
}
