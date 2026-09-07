#include "cli/add_cli_test_support.hpp"

#include <iostream>
#include <sstream>
#include <utility>

namespace howdy::test::add_cli {

	namespace {

		auto CaptureFailureResult(howdy::native::EnrollmentCaptureResult capture_result)
		    -> howdy::native::add_internal::AddEnrollmentResult {
			return howdy::native::add_internal::AddEnrollmentResult{
			    .status         = howdy::native::add_internal::AddEnrollmentStatus::kCaptureFailure,
			    .capture_result = std::move(capture_result),
			};
		}

		auto BlackFrameCaptureResult() -> howdy::native::add_internal::AddEnrollmentResult {
			howdy::native::EnrollmentCaptureResult capture_result;
			capture_result.black_frames = 1;
			return CaptureFailureResult(std::move(capture_result));
		}

		auto OnlyTooDarkCaptureResult() -> howdy::native::add_internal::AddEnrollmentResult {
			howdy::native::EnrollmentCaptureResult capture_result;
			capture_result.valid_frames       = 2;
			capture_result.dark_tries         = 2;
			capture_result.black_frames       = 0;
			capture_result.empty_frames       = 0;
			capture_result.read_failures      = 0;
			capture_result.dark_running_total = 80.0;
			return CaptureFailureResult(std::move(capture_result));
		}

		auto NoSufficientlyBrightCaptureResult()
		    -> howdy::native::add_internal::AddEnrollmentResult {
			howdy::native::EnrollmentCaptureResult capture_result;
			capture_result.valid_frames  = 1;
			capture_result.dark_tries    = 1;
			capture_result.black_frames  = 0;
			capture_result.empty_frames  = 1;
			capture_result.read_failures = 0;
			return CaptureFailureResult(std::move(capture_result));
		}

		auto NoUsableFramesCaptureResult() -> howdy::native::add_internal::AddEnrollmentResult {
			howdy::native::EnrollmentCaptureResult capture_result;
			capture_result.valid_frames  = 0;
			capture_result.read_failures = 1;
			return CaptureFailureResult(std::move(capture_result));
		}

		auto NoFaceDetectedCaptureResult() -> howdy::native::add_internal::AddEnrollmentResult {
			howdy::native::EnrollmentCaptureResult capture_result;
			capture_result.valid_frames = 1;
			capture_result.dark_tries   = 0;
			return CaptureFailureResult(std::move(capture_result));
		}

		auto EnrollmentFailureResult(howdy::native::add_internal::AddEnrollmentStatus status)
		    -> howdy::native::add_internal::AddEnrollmentResult {
			return howdy::native::add_internal::AddEnrollmentResult{
			    .status        = status,
			    .error_message = "enrollment failed",
			};
		}

		auto ExpectCaptureFailureStopsBeforeAppend(const AddCliTestContext &context, int result,
		                                           const std::string &test_name) -> bool {
			bool ok = true;
			ok &= expect(result == 1, test_name + " returns 1");
			ok &=
			    expect(context.preflight_calls == 1, test_name + " calls preflight callback once");
			ok &= expect(context.capture_calls == 1, test_name + " calls capture callback once");
			ok &= expect(context.append_calls == 0, test_name + " skips append callback");
			return ok;
		}

		auto EnrollmentFailureStatusStopsBeforeAppend(
		    howdy::native::add_internal::AddEnrollmentStatus status, const std::string &test_name)
		    -> bool {
			auto context           = MakeSuccessContext();
			context.capture_result = EnrollmentFailureResult(status);

			const int result = RunAdd(context, {"howdy-add", "alice", "front-door"});

			bool ok = true;
			ok &= expect(result == 1, test_name + " returns 1");
			ok &= expect(context.preflight_calls == 1, test_name + " calls preflight callback");
			ok &= expect(context.capture_calls == 1, test_name + " calls capture callback");
			ok &= expect(context.append_calls == 0, test_name + " skips append");
			return ok;
		}

		auto FaceModelEnrollmentFailureStopsBeforeAppend() -> bool {
			return EnrollmentFailureStatusStopsBeforeAppend(
			    howdy::native::add_internal::AddEnrollmentStatus::kFaceModelError,
			    "face-model enrollment failure");
		}

		auto CaptureOpenFailureStopsBeforeAppend() -> bool {
			return EnrollmentFailureStatusStopsBeforeAppend(
			    howdy::native::add_internal::AddEnrollmentStatus::kCaptureOpenError,
			    "capture open failure");
		}

		auto UnconfiguredCameraErrorIsPropagated() -> bool {
			auto context           = MakeSuccessContext();
			context.capture_result = EnrollmentFailureResult(
			    howdy::native::add_internal::AddEnrollmentStatus::kCaptureOpenError);
			context.capture_result.error_message =
			    "Camera is not configured; set video.device_path";
			std::ostringstream error;
			ErrorRedirect      error_redirect(std::cerr, error.rdbuf());

			const int result = RunAdd(context, {"howdy-add", "alice", "front-door"});

			bool ok = ExpectCaptureFailureStopsBeforeAppend(context, result, "unconfigured camera");
			ok &= expect(error.str() == context.capture_result.error_message + "\n",
			             "unconfigured camera prints only concise error in add");
			return ok;
		}

		auto BlackFrameCaptureFailureStopsBeforeAppend() -> bool {
			auto context           = MakeSuccessContext();
			context.capture_result = BlackFrameCaptureResult();
			std::ostringstream error;
			ErrorRedirect      error_redirect(std::cerr, error.rdbuf());

			const int result = RunAdd(context, {"howdy-add", "alice", "front-door"});

			const auto error_output = error.str();
			bool       ok = ExpectCaptureFailureStopsBeforeAppend(context, result,
			                                                      "black-frame capture failure");
			ok &= expect(
			    error_output.contains("Camera returned only black frames; check the IR emitter"),
			    "black-frame capture failure prints IR emitter diagnostic");
			return ok;
		}

		auto OnlyTooDarkCaptureFailurePrintsDiagnostic() -> bool {
			auto context           = MakeSuccessContext();
			context.capture_result = OnlyTooDarkCaptureResult();
			std::ostringstream error;
			ErrorRedirect      error_redirect(std::cerr, error.rdbuf());

			const int result = RunAdd(context, {"howdy-add", "alice", "front-door"});

			const auto error_output = error.str();
			bool       ok = ExpectCaptureFailureStopsBeforeAppend(context, result,
			                                                      "only-too-dark capture failure");
			ok &= expect(error_output.contains("All frames were too dark; check dark_threshold"),
			             "only-too-dark capture failure prints dark threshold diagnostic");
			ok &= expect(error_output.contains("Average darkness: 40, Threshold: 32"),
			             "only-too-dark capture failure prints average darkness and threshold");
			return ok;
		}

		auto NoSufficientlyBrightCaptureFailurePrintsDiagnostic() -> bool {
			auto context           = MakeSuccessContext();
			context.capture_result = NoSufficientlyBrightCaptureResult();
			std::ostringstream error;
			ErrorRedirect      error_redirect(std::cerr, error.rdbuf());

			const int result = RunAdd(context, {"howdy-add", "alice", "front-door"});

			const auto error_output = error.str();
			bool       ok           = ExpectCaptureFailureStopsBeforeAppend(
			    context, result, "no-sufficiently-bright capture failure");
			ok &= expect(error_output.contains("No sufficiently bright frames captured, aborting"),
			             "no-sufficiently-bright capture failure prints diagnostic");
			ok &=
			    expect(!error_output.contains("All frames were too dark; check dark_threshold"),
			           "no-sufficiently-bright capture failure does not print too-dark diagnostic");
			return ok;
		}

		auto NoUsableFramesCaptureFailurePrintsDiagnostic() -> bool {
			auto context           = MakeSuccessContext();
			context.capture_result = NoUsableFramesCaptureResult();
			std::ostringstream error;
			ErrorRedirect      error_redirect(std::cerr, error.rdbuf());

			const int result = RunAdd(context, {"howdy-add", "alice", "front-door"});

			const auto error_output = error.str();
			bool ok = ExpectCaptureFailureStopsBeforeAppend(context, result,
			                                                "no-usable-frames capture failure");
			ok &= expect(error_output.contains("No usable frames captured, aborting"),
			             "no-usable-frames capture failure prints diagnostic");
			return ok;
		}

		auto NoFaceDetectedCaptureFailurePrintsDiagnostic() -> bool {
			auto context           = MakeSuccessContext();
			context.capture_result = NoFaceDetectedCaptureResult();
			std::ostringstream error;
			ErrorRedirect      error_redirect(std::cerr, error.rdbuf());

			const int result = RunAdd(context, {"howdy-add", "alice", "front-door"});

			const auto error_output = error.str();
			bool ok = ExpectCaptureFailureStopsBeforeAppend(context, result,
			                                                "no-face-detected capture failure");
			ok &= expect(error_output.contains("No face detected, aborting"),
			             "no-face-detected capture failure prints diagnostic");
			return ok;
		}

		auto MultipleFacesFailureStopsBeforeAppend() -> bool {
			return EnrollmentFailureStatusStopsBeforeAppend(
			    howdy::native::add_internal::AddEnrollmentStatus::kMultipleFaces,
			    "multiple-faces failure");
		}

		auto EncodingFailureStopsBeforeAppend() -> bool {
			auto context           = MakeSuccessContext();
			context.capture_result = howdy::native::add_internal::AddEnrollmentResult{
			    .status        = howdy::native::add_internal::AddEnrollmentStatus::kEncodingError,
			    .error_message = "SFace feature extraction failed: synthetic failure",
			};
			std::ostringstream error;
			ErrorRedirect      error_redirect(std::cerr, error.rdbuf());

			const int result = RunAdd(context, {"howdy-add", "alice", "front-door"});

			bool ok = true;
			ok &= expect(result == 1, "encoding failure returns 1");
			ok &= expect(context.append_calls == 0, "encoding failure skips append");
			ok &= expect(error.str().contains(context.capture_result.error_message),
			             "encoding failure prints actionable diagnostic");
			return ok;
		}

		auto UnknownEnrollmentStatusFailsClosedBeforeAppend() -> bool {
			auto context           = MakeSuccessContext();
			context.capture_result = howdy::native::add_internal::AddEnrollmentResult{
			    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
			    .status = static_cast<howdy::native::add_internal::AddEnrollmentStatus>(99),
			};
			std::ostringstream error;
			ErrorRedirect      error_redirect(std::cerr, error.rdbuf());

			const int result = RunAdd(context, {"howdy-add", "alice", "front-door"});

			bool ok = true;
			ok &= expect(result == 1, "unknown enrollment status returns 1");
			ok &= expect(context.preflight_calls == 1, "unknown enrollment status calls preflight");
			ok &= expect(context.capture_calls == 1, "unknown enrollment status calls capture");
			ok &= expect(context.append_calls == 0, "unknown enrollment status skips append");
			ok &= expect(error.str().contains("Internal error: unknown add enrollment status"),
			             "unknown enrollment status writes internal error");
			return ok;
		}

	}  // namespace

	auto RunAddCliCaptureTests() -> bool {
		bool ok = true;
		ok &= FaceModelEnrollmentFailureStopsBeforeAppend();
		ok &= CaptureOpenFailureStopsBeforeAppend();
		ok &= UnconfiguredCameraErrorIsPropagated();
		ok &= BlackFrameCaptureFailureStopsBeforeAppend();
		ok &= OnlyTooDarkCaptureFailurePrintsDiagnostic();
		ok &= NoSufficientlyBrightCaptureFailurePrintsDiagnostic();
		ok &= NoUsableFramesCaptureFailurePrintsDiagnostic();
		ok &= NoFaceDetectedCaptureFailurePrintsDiagnostic();
		ok &= MultipleFacesFailureStopsBeforeAppend();
		ok &= EncodingFailureStopsBeforeAppend();
		ok &= UnknownEnrollmentStatusFailsClosedBeforeAppend();
		return ok;
	}

}  // namespace howdy::test::add_cli
