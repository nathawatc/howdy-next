#pragma once

#include "cli/add/enrollment_capture.hpp"
#include "config/runtime_config_loader.hpp"
#include "storage/user_models.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace howdy::native::add_internal {

	enum class AddPreflightStatus : std::uint8_t {
		kOk,
		kFaceModelError,
		kExistingModelIncompatible,
		kExistingModelError,
	};

	struct AddPreflightResult {
		AddPreflightStatus status = AddPreflightStatus::kFaceModelError;
		std::string        error_message;
	};

	enum class AddEnrollmentStatus : std::uint8_t {
		kOk,
		kFaceModelError,
		kCaptureOpenError,
		kCaptureFailure,
		kMultipleFaces,
		kEncodingError,
	};

	struct AddEnrollmentResult {
		AddEnrollmentStatus                    status = AddEnrollmentStatus::kCaptureFailure;
		std::string                            error_message;
		howdy::native::EnrollmentCaptureResult capture_result;
		howdy::native::FaceMetric              metric = howdy::native::FaceMetric::kCosine;
		std::vector<float>                     encoding;
	};

	using LoadRuntimeConfigFn   = howdy::native::RuntimeConfigLoadResult (*)(void *context);
	using PreflightEnrollmentFn = AddPreflightResult (*)(
	    void *context, const std::string &user, const howdy::native::RuntimeConfig &config);
	using CaptureEnrollmentFn = AddEnrollmentResult (*)(void *context, const std::string &user,
	                                                    const howdy::native::RuntimeConfig &config,
	                                                    bool plain, const std::string &label);
	using AppendUserModelEntryFn = howdy::native::UserModelMutationResult (*)(
	    void *context, const std::string &user, const howdy::native::NewUserModelEntry &entry);

	struct AddDependencies {
		void                  *context              = nullptr;
		LoadRuntimeConfigFn    load_runtime_config  = nullptr;
		PreflightEnrollmentFn  preflight_enrollment = nullptr;
		CaptureEnrollmentFn    capture_enrollment   = nullptr;
		AppendUserModelEntryFn append_user_model    = nullptr;
	};

	auto AddMainWithDependencies(int argc, char **argv, const AddDependencies &dependencies) -> int;

}  // namespace howdy::native::add_internal
