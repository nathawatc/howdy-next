#include "cli/add/enrollment_capture.hpp"
#include "cli/add/internal.hpp"
#include "config/runtime_config.hpp"
#include "storage/user_models.hpp"
#include "support/user_names.hpp"
#include "vision/face_model.hpp"

#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

	constexpr auto kExitOk    = 0;
	constexpr auto kExitAbort = 1;

	struct AddArgs {
		std::string user;
		std::string label;
		bool        label_provided = false;
		bool        plain          = false;
		bool        yes            = false;
	};

	auto parse_args(int argc, char **argv) -> std::optional<AddArgs> {
		AddArgs args;
		if (argc < 2) {
			std::cerr << "Usage: howdy-add <user> [label] [--plain] [-y]\n";
			return std::nullopt;
		}

		args.user          = argv[1];
		bool options_ended = false;
		for (int index = 2; index < argc; ++index) {
			const std::string_view arg(argv[index]);
			if (!options_ended && arg == "--") {
				options_ended = true;
				continue;
			}
			if (!options_ended && arg == "--plain") {
				args.plain = true;
				continue;
			}
			if (!options_ended && arg == "-y") {
				args.yes = true;
				continue;
			}
			if (!options_ended && !arg.empty() && arg.front() == '-') {
				std::cerr << "Unknown option: " << arg << "\n";
				return std::nullopt;
			}
			if (args.label_provided) {
				std::cerr << "Too many positional arguments for add\n";
				return std::nullopt;
			}
			args.label          = arg;
			args.label_provided = true;
		}
		return args;
	}

	void print_capture_failure(const howdy::native::EnrollmentCaptureResult &capture_result,
	                           float                                         dark_threshold) {
		switch (howdy::native::classify_enrollment_capture_failure(capture_result)) {
			case howdy::native::EnrollmentCaptureFailure::kOnlyBlackFrames:
				std::cerr << "Camera returned only black frames; check the IR emitter\n";
				break;
			case howdy::native::EnrollmentCaptureFailure::kOnlyTooDarkFrames:
				std::cerr << howdy::native::kAllFramesTooDarkMessage << '\n';
				std::cerr << howdy::native::kAverageDarknessLabel
				          << (capture_result.dark_running_total / capture_result.valid_frames)
				          << howdy::native::kThresholdLabel << dark_threshold << '\n';
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
	}

}  // namespace

auto howdy::native::add_internal::add_main_with_dependencies(int argc, char **argv,
                                                             const AddDependencies &dependencies)
    -> int {
	if (dependencies.load_runtime_config == nullptr ||
	    dependencies.preflight_enrollment == nullptr ||
	    dependencies.capture_enrollment == nullptr || dependencies.append_user_model == nullptr) {
		return kExitAbort;
	}

	const auto args = parse_args(argc, argv);
	if (!args.has_value()) {
		return kExitAbort;
	}
	if (args->label_provided && !howdy::native::is_valid_model_label(args->label)) {
		std::cerr << "Invalid model label\n";
		return kExitAbort;
	}

	auto config_result = dependencies.load_runtime_config(dependencies.context);
	if (config_result.status != howdy::native::RuntimeConfigLoadStatus::kOk ||
	    !config_result.config.has_value()) {
		std::cerr << config_result.error_message << "\n";
		return kExitAbort;
	}
	const auto &config = *config_result.config;

	auto preflight_result =
	    dependencies.preflight_enrollment(dependencies.context, args->user, config);
	switch (preflight_result.status) {
		case AddPreflightStatus::kOk:
			break;
		case AddPreflightStatus::kFaceModelError:
		case AddPreflightStatus::kExistingModelError:
			std::cerr << preflight_result.error_message << "\n";
			return kExitAbort;
		case AddPreflightStatus::kExistingModelIncompatible:
			std::cerr << "Existing face models use incompatible face-recognition metadata.\n";
			std::cerr << "Please run `howdy clear` and enroll again with `howdy add`.\n";
			return kExitAbort;
		default:
			std::cerr << "Internal error: unknown add preflight status\n";
			return kExitAbort;
	}

	std::string label = args->label;
	if (!args->yes && args->label.empty() && !args->plain) {
		std::cout << "Enter a label for this new model [automatic]: ";
		std::string input;
		std::getline(std::cin, input);
		if (!input.empty()) {
			label = input.substr(0, 24);
		}
	}
	if (!howdy::native::is_valid_model_label(label)) {
		std::cerr << "Invalid model label\n";
		return kExitAbort;
	}

	auto enrollment_result = dependencies.capture_enrollment(dependencies.context, args->user,
	                                                         config, args->plain, label);
	switch (enrollment_result.status) {
		case AddEnrollmentStatus::kOk:
			break;
		case AddEnrollmentStatus::kFaceModelError:
		case AddEnrollmentStatus::kCaptureOpenError:
			std::cerr << enrollment_result.error_message << "\n";
			return kExitAbort;
		case AddEnrollmentStatus::kCaptureFailure:
			print_capture_failure(enrollment_result.capture_result, config.video.dark_threshold);
			return kExitAbort;
		case AddEnrollmentStatus::kMultipleFaces:
			std::cerr << "Multiple faces detected, aborting\n";
			return kExitAbort;
		case AddEnrollmentStatus::kEncodingError:
			std::cerr << enrollment_result.error_message << "\n";
			return kExitAbort;
		default:
			std::cerr << "Internal error: unknown add enrollment status\n";
			return kExitAbort;
	}

	const auto append_result =
	    dependencies.append_user_model(dependencies.context, args->user,
	                                   howdy::native::NewUserModelEntry{
	                                       .label     = label,
	                                       .backend   = howdy::native::FaceModel::kBackendName,
	                                       .metric    = enrollment_result.metric,
	                                       .model     = howdy::native::FaceModel::kSfaceModel,
	                                       .encodings = {std::move(enrollment_result.encoding)},
	                                   });
	if (append_result.status != howdy::native::UserModelStatus::kOk) {
		std::cerr << append_result.error_message << "\n";
		return kExitAbort;
	}

	std::cout << "\nScan complete\nAdded a new model to " << args->user << "\n";
	return kExitOk;
}
