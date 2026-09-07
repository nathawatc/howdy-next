#include "cli/add.hpp"

#include "cli/add/enrollment_capture.hpp"
#include "cli/add/internal.hpp"
#include "config/runtime_config.hpp"
#include "storage/user_models.hpp"
#include "vision/face_model.hpp"
#include "vision/video_capture.hpp"

#include <chrono>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace {

	constexpr int kMaxFrames = 60;

	struct AddProductionContext {
		std::optional<howdy::native::FaceModel> face_model;
	};

	auto add_cli_load_runtime_config_dependency(void *context)
	    -> howdy::native::RuntimeConfigLoadResult {
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
			    .error_message = howdy::native::kFaceModelNotInitializedMessage,
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
			    .error_message = howdy::native::kFaceModelNotInitializedMessage,
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

		auto encoding_result =
		    face_model.encode(capture_result.frame, capture_result.faces.front());
		if (!encoding_result.ok()) {
			return howdy::native::add_internal::AddEnrollmentResult{
			    .status         = howdy::native::add_internal::AddEnrollmentStatus::kEncodingError,
			    .error_message  = std::move(encoding_result.error_message),
			    .capture_result = std::move(capture_result),
			};
		}

		return howdy::native::add_internal::AddEnrollmentResult{
		    .status         = howdy::native::add_internal::AddEnrollmentStatus::kOk,
		    .capture_result = std::move(capture_result),
		    .metric         = face_model.metric(),
		    .encoding       = std::move(encoding_result.encoding),
		};
	}

}  // namespace

auto add_main(int argc, char **argv) -> int {
	AddProductionContext context;
	return howdy::native::add_internal::add_main_with_dependencies(
	    argc, argv,
	    howdy::native::add_internal::AddDependencies{
	        .context              = &context,
	        .load_runtime_config  = add_cli_load_runtime_config_dependency,
	        .preflight_enrollment = preflight_enrollment_dependency,
	        .capture_enrollment   = capture_enrollment_dependency,
	        .append_user_model    = append_user_model_entry_dependency,
	    });
}
