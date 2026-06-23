#include "cli/add_cli.hpp"
#include "cli/enrollment_capture.hpp"
#include "config/runtime_config.hpp"
#include "core/face_model.hpp"
#include "recorders/video_capture.hpp"
#include "storage/user_models.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

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

	const float dark_threshold = config.video.dark_threshold;

	if (!args.plain) {
		std::cout << "\nPlease look straight into the camera\n";
	}
	std::this_thread::sleep_for(std::chrono::seconds(2));

	auto capture_result =
	    howdy::native::capture_enrollment_sample(capture, face_model, config.video, kMaxFrames);

	capture.release();

	if (capture_result.faces.empty()) {
		switch (howdy::native::classify_enrollment_capture_failure(capture_result)) {
			case howdy::native::EnrollmentCaptureFailure::kOnlyBlackFrames:
				std::cerr << "Camera saw only black frames - is IR emitter working?\n";
				break;
			case howdy::native::EnrollmentCaptureFailure::kOnlyTooDarkFrames:
				std::cerr << "All frames were too dark, please check dark_threshold in config\n";
				std::cerr << "Average darkness: "
				          << (capture_result.dark_running_total / capture_result.valid_frames)
				          << ", Threshold: " << dark_threshold << "\n";
				break;
			case howdy::native::EnrollmentCaptureFailure::kNoSufficientlyBrightFrames:
				std::cerr << "No sufficiently bright frames captured, aborting\n";
				break;
			case howdy::native::EnrollmentCaptureFailure::kNoUsableFrames:
				std::cerr << "No usable frames captured, aborting\n";
				break;
			case howdy::native::EnrollmentCaptureFailure::kNoFaceDetected:
				std::cerr << "No face detected, aborting\n";
				break;
		}
		return kExitAbort;
	}

	if (capture_result.faces.size() > 1) {
		std::cerr << "Multiple faces detected, aborting\n";
		return kExitAbort;
	}

	auto encoding = face_model.encode(capture_result.frame, capture_result.faces.front());
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
