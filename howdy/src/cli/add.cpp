#include "cli/add_cli.hpp"
#include "cli/add_internal.hpp"
#include "cli/enrollment_capture.hpp"
#include "config/runtime_config.hpp"
#include "core/face_model.hpp"
#include "recorders/video_capture.hpp"
#include "storage/user_models.hpp"

#include <chrono>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

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

	struct AddProductionContext {
		std::optional<howdy::native::FaceModel> face_model;
	};

	auto parse_args(int argc, char **argv) -> std::optional<AddArgs> {
		AddArgs args;
		if (argc < 2) {
			std::cerr << "Usage: howdy-add <user> [label] [--plain] [-y]\n";
			return std::nullopt;
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

	auto load_runtime_config_dependency(void *context) -> howdy::native::RuntimeConfigLoadResult {
		(void)context;
		return howdy::native::load_runtime_config();
	}

	auto append_user_model_entry_dependency(void *context, const std::string &user,
	                                        const howdy::native::NewUserModelEntry &entry)
	    -> howdy::native::UserModelMutationResult {
		(void)context;
		return howdy::native::append_user_model_entry(user, entry);
	}

	auto is_existing_model_incompatible_status(howdy::native::UserModelStatus status) -> bool {
		return status == howdy::native::UserModelStatus::kIncompatibleBackend ||
		       status == howdy::native::UserModelStatus::kIncompatibleMetric ||
		       status == howdy::native::UserModelStatus::kIncompatibleModel;
	}

	auto is_existing_model_allowed_status(howdy::native::UserModelStatus status) -> bool {
		return status == howdy::native::UserModelStatus::kOk ||
		       status == howdy::native::UserModelStatus::kNoModel ||
		       status == howdy::native::UserModelStatus::kNoModelDirectory;
	}

	auto preflight_enrollment_dependency(void *context, const std::string &user,
	                                     const howdy::native::RuntimeConfig &config)
	    -> howdy::native::add_internal::AddPreflightResult {
		auto *production_context = static_cast<AddProductionContext *>(context);
		if (production_context == nullptr) {
			return howdy::native::add_internal::AddPreflightResult{
			    .status        = howdy::native::add_internal::AddPreflightStatus::kFaceModelError,
			    .error_message = "Face model was not initialized",
			};
		}

		auto &face_model = production_context->face_model.emplace(config.face);
		if (!face_model.ok()) {
			return howdy::native::add_internal::AddPreflightResult{
			    .status        = howdy::native::add_internal::AddPreflightStatus::kFaceModelError,
			    .error_message = face_model.error_message(),
			};
		}

		const auto entries = howdy::native::list_user_model_entries(
		    user, howdy::native::FaceModel::kBackendName, face_model.metric(),
		    howdy::native::FaceModel::kSfaceModel);
		if (is_existing_model_incompatible_status(entries.status)) {
			return howdy::native::add_internal::AddPreflightResult{
			    .status =
			        howdy::native::add_internal::AddPreflightStatus::kExistingModelIncompatible,
			};
		}
		if (!is_existing_model_allowed_status(entries.status)) {
			return howdy::native::add_internal::AddPreflightResult{
			    .status = howdy::native::add_internal::AddPreflightStatus::kExistingModelError,
			    .error_message = entries.error_message,
			};
		}

		return howdy::native::add_internal::AddPreflightResult{
		    .status = howdy::native::add_internal::AddPreflightStatus::kOk,
		};
	}

	auto capture_enrollment_dependency(void *context, const std::string &user,
	                                   const howdy::native::RuntimeConfig &config, bool plain,
	                                   const std::string &label)
	    -> howdy::native::add_internal::AddEnrollmentResult {
		(void)user;
		(void)label;

		auto *production_context = static_cast<AddProductionContext *>(context);
		if (production_context == nullptr || !production_context->face_model.has_value()) {
			return howdy::native::add_internal::AddEnrollmentResult{
			    .status        = howdy::native::add_internal::AddEnrollmentStatus::kFaceModelError,
			    .error_message = "Face model was not initialized",
			};
		}

		auto &face_model = *production_context->face_model;
		if (!face_model.ok()) {
			return howdy::native::add_internal::AddEnrollmentResult{
			    .status        = howdy::native::add_internal::AddEnrollmentStatus::kFaceModelError,
			    .error_message = face_model.error_message(),
			};
		}

		howdy::native::VideoCapture capture(howdy::native::load_capture_settings(config.video));
		if (!capture.open()) {
			return howdy::native::add_internal::AddEnrollmentResult{
			    .status = howdy::native::add_internal::AddEnrollmentStatus::kCaptureOpenError,
			    .error_message = capture.error_message(),
			};
		}

		if (!plain) {
			std::cout << "\nPlease look straight into the camera\n";
		}
		std::this_thread::sleep_for(std::chrono::seconds(2));

		auto capture_result =
		    howdy::native::capture_enrollment_sample(capture, face_model, config.video, kMaxFrames);

		capture.release();

		if (capture_result.detector_status != howdy::native::FaceDetectionStatus::kOk) {
			return howdy::native::add_internal::AddEnrollmentResult{
			    .status         = howdy::native::add_internal::AddEnrollmentStatus::kFaceModelError,
			    .error_message  = capture_result.detector_error_message,
			    .capture_result = std::move(capture_result),
			};
		}

		if (capture_result.faces.empty()) {
			return howdy::native::add_internal::AddEnrollmentResult{
			    .status         = howdy::native::add_internal::AddEnrollmentStatus::kCaptureFailure,
			    .capture_result = std::move(capture_result),
			};
		}

		if (capture_result.faces.size() > 1) {
			return howdy::native::add_internal::AddEnrollmentResult{
			    .status         = howdy::native::add_internal::AddEnrollmentStatus::kMultipleFaces,
			    .capture_result = std::move(capture_result),
			};
		}

		auto encoding = face_model.encode(capture_result.frame, capture_result.faces.front());
		if (encoding.empty()) {
			return howdy::native::add_internal::AddEnrollmentResult{
			    .status         = howdy::native::add_internal::AddEnrollmentStatus::kEncodingError,
			    .capture_result = std::move(capture_result),
			};
		}

		return howdy::native::add_internal::AddEnrollmentResult{
		    .status         = howdy::native::add_internal::AddEnrollmentStatus::kOk,
		    .capture_result = std::move(capture_result),
		    .metric         = face_model.metric(),
		    .encoding       = std::move(encoding),
		};
	}

	void print_capture_failure(const howdy::native::EnrollmentCaptureResult &capture_result,
	                           float                                         dark_threshold) {
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
	std::erase(label, ',');

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
			std::cerr << "No valid face encoding captured\n";
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

auto add_main(int argc, char **argv) -> int {
	AddProductionContext context;
	return howdy::native::add_internal::add_main_with_dependencies(
	    argc, argv,
	    howdy::native::add_internal::AddDependencies{
	        .context              = &context,
	        .load_runtime_config  = load_runtime_config_dependency,
	        .preflight_enrollment = preflight_enrollment_dependency,
	        .capture_enrollment   = capture_enrollment_dependency,
	        .append_user_model    = append_user_model_entry_dependency,
	    });
}
