#include "cli/add.hpp"

#include "cli/add/enrollment_capture.hpp"
#include "cli/add/internal.hpp"
#include "config/runtime_config.hpp"
#include "storage/user_model_status.hpp"
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

	auto AddCliLoadRuntimeConfigDependency(void *context)
	    -> howdy::native::RuntimeConfigLoadResult {
		(void)context;
		return howdy::native::LoadRuntimeConfig();
	}

	auto AppendUserModelEntryDependency(void *context, const std::string &user,
	                                    const howdy::native::NewUserModelEntry &entry)
	    -> howdy::native::UserModelMutationResult {
		(void)context;
		return howdy::native::AppendUserModelEntry(user, entry);
	}

	auto IsExistingModelIncompatibleStatus(howdy::native::UserModelStatus status) -> bool {
		return status == howdy::native::UserModelStatus::kIncompatibleBackend ||
		       status == howdy::native::UserModelStatus::kIncompatibleMetric ||
		       status == howdy::native::UserModelStatus::kIncompatibleModel;
	}

	auto IsExistingModelAllowedStatus(howdy::native::UserModelStatus status) -> bool {
		return status == howdy::native::UserModelStatus::kOk ||
		       status == howdy::native::UserModelStatus::kNoModel ||
		       status == howdy::native::UserModelStatus::kNoModelDirectory;
	}

	auto PreflightEnrollmentDependency(void *context, const std::string &user,
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
		if (!face_model.Ok()) {
			return howdy::native::add_internal::AddPreflightResult{
			    .status        = howdy::native::add_internal::AddPreflightStatus::kFaceModelError,
			    .error_message = face_model.ErrorMessage(),
			};
		}

		const auto entries = howdy::native::ListUserModelEntries(
		    user, howdy::native::FaceModel::kBackendName, face_model.Metric(),
		    howdy::native::FaceModel::kSfaceModel);
		if (IsExistingModelIncompatibleStatus(entries.status)) {
			return howdy::native::add_internal::AddPreflightResult{
			    .status =
			        howdy::native::add_internal::AddPreflightStatus::kExistingModelIncompatible,
			};
		}
		if (!IsExistingModelAllowedStatus(entries.status)) {
			return howdy::native::add_internal::AddPreflightResult{
			    .status = howdy::native::add_internal::AddPreflightStatus::kExistingModelError,
			    .error_message = entries.error_message,
			};
		}

		return howdy::native::add_internal::AddPreflightResult{
		    .status = howdy::native::add_internal::AddPreflightStatus::kOk,
		};
	}

	auto CaptureEnrollmentDependency(void *context, const std::string &user,
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
		if (!face_model.Ok()) {
			return howdy::native::add_internal::AddEnrollmentResult{
			    .status        = howdy::native::add_internal::AddEnrollmentStatus::kFaceModelError,
			    .error_message = face_model.ErrorMessage(),
			};
		}

		howdy::native::VideoCapture capture(howdy::native::LoadCaptureSettings(config.video));
		if (!capture.Open()) {
			return howdy::native::add_internal::AddEnrollmentResult{
			    .status = howdy::native::add_internal::AddEnrollmentStatus::kCaptureOpenError,
			    .error_message = capture.ErrorMessage(),
			};
		}

		if (!plain) {
			std::cout << "\nPlease look straight into the camera\n";
		}
		std::this_thread::sleep_for(std::chrono::seconds(2));

		auto capture_result =
		    howdy::native::CaptureEnrollmentSample(capture, face_model, config.video, kMaxFrames);

		capture.Release();

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
		    face_model.Encode(capture_result.frame, capture_result.faces.front());
		if (!encoding_result.Ok()) {
			return howdy::native::add_internal::AddEnrollmentResult{
			    .status         = howdy::native::add_internal::AddEnrollmentStatus::kEncodingError,
			    .error_message  = std::move(encoding_result.error_message),
			    .capture_result = std::move(capture_result),
			};
		}

		return howdy::native::add_internal::AddEnrollmentResult{
		    .status         = howdy::native::add_internal::AddEnrollmentStatus::kOk,
		    .capture_result = std::move(capture_result),
		    .metric         = face_model.Metric(),
		    .encoding       = std::move(encoding_result.encoding),
		};
	}

}  // namespace

auto AddMain(int argc, char **argv) -> int {
	AddProductionContext context;
	return howdy::native::add_internal::AddMainWithDependencies(
	    argc, argv,
	    howdy::native::add_internal::AddDependencies{
	        .context              = &context,
	        .load_runtime_config  = AddCliLoadRuntimeConfigDependency,
	        .preflight_enrollment = PreflightEnrollmentDependency,
	        .capture_enrollment   = CaptureEnrollmentDependency,
	        .append_user_model    = AppendUserModelEntryDependency,
	    });
}
