#include "cli/add/enrollment_capture.hpp"
#include "cli/add/internal.hpp"
#include "config/runtime_config_loader.hpp"
#include "storage/user_model_status.hpp"
#include "storage/user_model_types.hpp"
#include "support/user_names.hpp"
#include "vision/face_model.hpp"

#include <iostream>
#include <string>
#include <utility>

namespace {

	constexpr auto kAddExitOk    = 0;
	constexpr auto kAddExitAbort = 1;

	struct AddArgs {
		std::string user;
		std::string label;
		bool        label_provided = false;
		bool        plain          = false;
		bool        yes            = false;
	};

	void PrintCaptureFailure(const howdy::native::EnrollmentCaptureResult &capture_result,
	                         float                                         dark_threshold) {
		switch (howdy::native::ClassifyEnrollmentCaptureFailure(capture_result)) {
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

auto howdy::native::add_internal::AddMainWithDependencies(
    const howdy::native::CommandInvocation &invocation, const AddDependencies &dependencies)
    -> int {
	if (dependencies.load_runtime_config == nullptr ||
	    dependencies.preflight_enrollment == nullptr ||
	    dependencies.capture_enrollment == nullptr || dependencies.append_user_model == nullptr) {
		return kAddExitAbort;
	}

	if (!invocation.resolved_user.has_value()) {
		return kAddExitAbort;
	}
	const AddArgs args{
	    .user  = *invocation.resolved_user,
	    .label = invocation.positionals.empty() ? std::string{} : invocation.positionals.front(),
	    .label_provided = !invocation.positionals.empty(),
	    .plain          = invocation.plain,
	    .yes            = invocation.assume_yes,
	};
	if (args.label_provided && !howdy::native::IsValidModelLabel(args.label)) {
		std::cerr << "Invalid model label\n";
		return kAddExitAbort;
	}

	auto config_result = dependencies.load_runtime_config(dependencies.context);
	if (config_result.status != howdy::native::RuntimeConfigLoadStatus::kOk ||
	    !config_result.config.has_value()) {
		std::cerr << config_result.error_message << "\n";
		return kAddExitAbort;
	}
	const auto &config = *config_result.config;

	auto preflight_result =
	    dependencies.preflight_enrollment(dependencies.context, args.user, config);
	switch (preflight_result.status) {
		case AddPreflightStatus::kOk:
			break;
		case AddPreflightStatus::kFaceModelError:
		case AddPreflightStatus::kExistingModelError:
			std::cerr << preflight_result.error_message << "\n";
			return kAddExitAbort;
		case AddPreflightStatus::kExistingModelIncompatible:
			std::cerr << "Existing face models use incompatible face-recognition metadata.\n";
			std::cerr << "Please run `howdy clear` and enroll again with `howdy add`.\n";
			return kAddExitAbort;
		default:
			std::cerr << "Internal error: unknown add preflight status\n";
			return kAddExitAbort;
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
	if (!howdy::native::IsValidModelLabel(label)) {
		std::cerr << "Invalid model label\n";
		return kAddExitAbort;
	}

	auto enrollment_result =
	    dependencies.capture_enrollment(dependencies.context, args.user, config, args.plain, label);
	switch (enrollment_result.status) {
		case AddEnrollmentStatus::kOk:
			break;
		case AddEnrollmentStatus::kFaceModelError:
		case AddEnrollmentStatus::kCaptureOpenError:
			std::cerr << enrollment_result.error_message << "\n";
			return kAddExitAbort;
		case AddEnrollmentStatus::kCaptureFailure:
			PrintCaptureFailure(enrollment_result.capture_result, config.video.dark_threshold);
			return kAddExitAbort;
		case AddEnrollmentStatus::kMultipleFaces:
			std::cerr << "Multiple faces detected, aborting\n";
			return kAddExitAbort;
		case AddEnrollmentStatus::kEncodingError:
			std::cerr << enrollment_result.error_message << "\n";
			return kAddExitAbort;
		default:
			std::cerr << "Internal error: unknown add enrollment status\n";
			return kAddExitAbort;
	}

	const auto append_result =
	    dependencies.append_user_model(dependencies.context, args.user,
	                                   howdy::native::NewUserModelEntry{
	                                       .label     = label,
	                                       .backend   = howdy::native::FaceModel::kBackendName,
	                                       .metric    = enrollment_result.metric,
	                                       .model     = howdy::native::FaceModel::kSfaceModel,
	                                       .encodings = {std::move(enrollment_result.encoding)},
	                                   });
	if (append_result.status != howdy::native::UserModelStatus::kOk) {
		std::cerr << append_result.error_message << "\n";
		return kAddExitAbort;
	}

	std::cout << "\nScan complete\nAdded a new model to " << args.user << "\n";
	return kAddExitOk;
}
