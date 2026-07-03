#pragma once

#include "config/runtime_config.hpp"
#include "core/face_detection.hpp"
#include "core/face_encoding.hpp"
#include "core/face_matching.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <opencv2/core.hpp>

namespace howdy::native {
	enum class BrightnessDecision;
}

namespace howdy::native::test_cli_internal {

	enum class TestPreviewStatus {
		kOk,
		kFaceModelError,
		kMissingGraphicalEnvironment,
		kCameraOpenError,
		kCameraReadError,
		kGuiUserError,
	};

	struct TestPreviewResult {
		TestPreviewStatus status = TestPreviewStatus::kFaceModelError;
		std::string       error_message;
		std::string       device_path;
	};

	struct PreviewBrightnessPresentation {
		const char *frame_label  = "";
		bool        detect_faces = false;
	};

	using EncodePreviewFaceFn = howdy::native::FaceEncodingResult (*)(
	    void *context, const cv::Mat &frame, const howdy::native::FaceDetection &face);
	using MatchPreviewFaceFn =
	    howdy::native::FaceMatch (*)(void *context, const std::vector<std::vector<float>> &known,
	                                 const std::vector<float> &probe);

	struct PreviewFaceMatchingDependencies {
		void               *context     = nullptr;
		EncodePreviewFaceFn encode_face = nullptr;
		MatchPreviewFaceFn  match_face  = nullptr;
	};

	struct PreviewFaceMatchingResult {
		TestPreviewStatus status = TestPreviewStatus::kFaceModelError;
		std::vector<std::optional<howdy::native::FaceMatch>> matches;
		std::string                                          error_message;

		[[nodiscard]] auto ok() const -> bool {
			return status == TestPreviewStatus::kOk;
		}
	};

	enum class PreviewFaceDisplayState {
		kEncodingFailed,
		kNoMatch,
		kMatch,
	};

	struct TestPreflightOperationResult {
		bool        ok = false;
		std::string error_message;
	};

	using LoadRuntimeConfigFn = howdy::native::RuntimeConfigLoadResult (*)(void *context);
	using FaceModelReadyFn    = TestPreflightOperationResult (*)(
	    void *context, const howdy::native::RuntimeConfig &config, const std::string &user);
	using HasGraphicalDisplayFn = bool (*)(void *context);
	using OpenCameraFn          = TestPreflightOperationResult (*)(
	    void *context, const howdy::native::RuntimeConfig &config, const std::string &device_path);
	using ReadCameraFn    = bool (*)(void *context);
	using SwitchGuiUserFn = bool (*)(void *context);
	using InitializeGuiFn = void (*)(void *context);

	struct TestPreviewPreflightDependencies {
		void                 *context               = nullptr;
		FaceModelReadyFn      face_model_ready      = nullptr;
		HasGraphicalDisplayFn has_graphical_display = nullptr;
		OpenCameraFn          open_camera           = nullptr;
		ReadCameraFn          read_camera           = nullptr;
		SwitchGuiUserFn       switch_gui_user       = nullptr;
		InitializeGuiFn       initialize_gui        = nullptr;
	};

	using RunPreviewFn = TestPreviewResult (*)(void                               *context,
	                                           const howdy::native::RuntimeConfig &config,
	                                           const std::string                  &user,
	                                           const std::string                  &device_path);

	struct TestDependencies {
		void               *context             = nullptr;
		LoadRuntimeConfigFn load_runtime_config = nullptr;
		RunPreviewFn        run_preview         = nullptr;
	};

	auto test_main_with_dependencies(int argc, char **argv, const TestDependencies &dependencies)
	    -> int;
	auto preview_brightness_presentation(howdy::native::BrightnessDecision decision)
	    -> PreviewBrightnessPresentation;
	auto validate_preview_gray_frame(const cv::Mat &gray_frame) -> TestPreviewResult;
	auto match_preview_faces(const cv::Mat                                   &frame,
	                         const std::vector<howdy::native::FaceDetection> &faces,
	                         const std::vector<std::vector<float>>           &known,
	                         const PreviewFaceMatchingDependencies           &dependencies)
	    -> PreviewFaceMatchingResult;
	auto preview_face_display_state(const std::optional<howdy::native::FaceMatch> &match)
	    -> PreviewFaceDisplayState;

	auto run_preview_preflight(const howdy::native::RuntimeConfig &config, const std::string &user,
	                           const std::string                      &device_path,
	                           const TestPreviewPreflightDependencies &dependencies)
	    -> TestPreviewResult;

	auto has_graphical_display_environment(std::string_view display,
	                                       std::string_view wayland_display,
	                                       std::string_view runtime_dir) -> bool;

}  // namespace howdy::native::test_cli_internal
