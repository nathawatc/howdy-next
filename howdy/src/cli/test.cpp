#include "cli/test.hpp"

#include "cli/test/internal.hpp"
#include "cli/test/preview_renderer.hpp"
#include "cli/test/preview_session.hpp"
#include "config/runtime_config_loader.hpp"
#include "storage/user_model_status.hpp"
#include "storage/user_models.hpp"
#include "support/capture_device_path.hpp"
#include "support/invoking_user.hpp"
#include "support/invoking_user_env.hpp"
#include "vision/face_model.hpp"
#include "vision/preview_engine.hpp"
#include "vision/video_capture.hpp"

#include <chrono>
#include <cstdlib>
#include <grp.h>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>

#include <opencv2/core.hpp>

namespace {

	constexpr int kTestExitOk      = 0;
	constexpr int kExitCameraError = 1;

	namespace test_cli_internal = howdy::native::test_cli_internal;

	struct TestProductionContext {
		std::optional<howdy::native::FaceModel>               face_model;
		std::optional<howdy::native::VideoCapture>            capture;
		std::optional<test_cli_internal::TestPreviewRenderer> renderer;
		howdy::native::UserModelLoadResult                    loaded_models;
		cv::Mat                                               prefetched_gray_frame;
		int                                                   exposure = -1;
	};

	auto PreparePreviewFrame(void *context, const cv::Mat &frame) -> cv::Mat {
		(void)context;
		return howdy::native::FaceModel::PrepareFrame(frame);
	}

	auto DetectPreviewFaces(void *context, const cv::Mat &frame)
	    -> howdy::native::FaceDetectionResult {
		return static_cast<howdy::native::FaceModel *>(context)->Detect(frame);
	}

	auto EncodePreviewFace(void *context, const cv::Mat &frame,
	                       const howdy::native::FaceDetection &face)
	    -> howdy::native::FaceEncodingResult {
		return static_cast<howdy::native::FaceModel *>(context)->Encode(frame, face);
	}

	auto MatchPreviewFace(void *context, const std::vector<std::vector<float>> &known,
	                      const std::vector<float> &probe) -> howdy::native::FaceMatch {
		return static_cast<howdy::native::FaceModel *>(context)->BestMatch(known, probe);
	}

	auto DropToInvokingGuiUser() -> bool {
		if (geteuid() != 0) {
			return true;
		}

		const auto invoking_identity = howdy::native::ResolveInvokingIdentity();
		if (invoking_identity.status != howdy::native::InvokingIdentityStatus::kResolved ||
		    !invoking_identity.user.has_value()) {
			return false;
		}

		const auto &invoking_user = *invoking_identity.user;
		howdy::native::ResetInvokingUserGuiEnvironment(invoking_user);
		return initgroups(invoking_user.name.c_str(), invoking_user.gid) == 0 &&
		       setgid(invoking_user.gid) == 0 && setuid(invoking_user.uid) == 0;
	}

	auto GetenvStringView(const char *name) -> std::string_view {
		const char *value = std::getenv(name);
		if (value == nullptr) {
			return {};
		}
		return value;
	}

	void PrepareInvokingGuiEnvironment() {
		if (geteuid() != 0) {
			return;
		}

		const auto invoking_identity = howdy::native::ResolveInvokingIdentity();
		if (invoking_identity.status == howdy::native::InvokingIdentityStatus::kResolved &&
		    invoking_identity.user.has_value()) {
			howdy::native::PrepareInvokingUserGuiEnvironment(*invoking_identity.user);
		}
	}

	auto HasGraphicalDisplayEnvironment() -> bool {
		return test_cli_internal::HasGraphicalDisplayEnvironment(
		    GetenvStringView("DISPLAY"),
		    GetenvStringView(howdy::native::kWaylandDisplayEnvironmentVariable),
		    GetenvStringView(howdy::native::kXdgRuntimeDirEnvironmentVariable));
	}

	void PrintMissingGraphicalEnvironmentDiagnostic() {
		std::cerr << "Cannot open the interactive test preview because no graphical display "
		             "environment is available.\n";
		std::cerr << "Howdy automatically detects a standard Wayland session for the invoking "
		             "user.\n\n";
		std::cerr
		    << "If multiple Wayland displays are active, pass the intended display explicitly:\n";
		std::cerr << "  run0 --setenv=" << howdy::native::kWaylandDisplayEnvironmentVariable
		          << " howdy test\n";
		std::cerr << "  sudo --preserve-env=" << howdy::native::kWaylandDisplayEnvironmentVariable
		          << " howdy test\n\n";
		std::cerr << "For X11, preserve its display credentials instead:\n";
		std::cerr << "  run0 --setenv=DISPLAY --setenv=XAUTHORITY howdy test\n";
		std::cerr << "  sudo --preserve-env=DISPLAY,XAUTHORITY howdy test\n\n";
		std::cerr << "For headless testing, use:\n";
		std::cerr << "  sudo howdy snapshot\n";
	}

	auto TestCliLoadRuntimeConfigDependency(void *context)
	    -> howdy::native::RuntimeConfigLoadResult {
		(void)context;
		return howdy::native::LoadRuntimeConfig();
	}

	auto FaceModelReadyDependency(void *context, const howdy::native::RuntimeConfig &config,
	                              const std::string &user)
	    -> test_cli_internal::TestPreflightOperationResult {
		auto *production_context = static_cast<TestProductionContext *>(context);
		if (production_context == nullptr) {
			return test_cli_internal::TestPreflightOperationResult{
			    .error_message = howdy::native::kFaceModelNotInitializedMessage,
			};
		}

		auto &face_model = production_context->face_model.emplace(config.face);
		if (!face_model.Ok()) {
			return test_cli_internal::TestPreflightOperationResult{
			    .error_message = face_model.ErrorMessage(),
			};
		}

		production_context->loaded_models = howdy::native::UserModelLoadResult{};
		if (!user.empty()) {
			production_context->loaded_models =
			    howdy::native::LoadUserModels(user, howdy::native::FaceModel::kBackendName);
			if (production_context->loaded_models.status ==
			    howdy::native::UserModelStatus::kIncompatibleBackend) {
				std::cout << "Warning: Stored face models use an incompatible backend; matching "
				             "disabled\n";
			} else if (production_context->loaded_models.status ==
			           howdy::native::UserModelStatus::kInvalidUser) {
				std::cout << "Warning: Invalid user name; matching disabled\n";
			} else if (production_context->loaded_models.status ==
			           howdy::native::UserModelStatus::kNoModel) {
				std::cout
				    << "Warning: No face model found for this user, detection will run without "
				       "matching\n";
			} else if (production_context->loaded_models.status ==
			           howdy::native::UserModelStatus::kParseError) {
				std::cout
				    << "Warning: Failed to read stored face models, detection will run without "
				       "matching\n";
			} else if (production_context->loaded_models.status ==
			           howdy::native::UserModelStatus::kInsecurePath) {
				std::cout
				    << "Warning: Stored face model path is insecure, detection will run without "
				       "matching\n";
			}
		}

		test_cli_internal::ReplaceTestPreviewRenderer(
		    production_context->renderer, config.video,
		    production_context->loaded_models.stored.models);
		return test_cli_internal::TestPreflightOperationResult{.ok = true};
	}

	auto HasGraphicalDisplayDependency(void *context) -> bool {
		(void)context;
		return HasGraphicalDisplayEnvironment();
	}

	auto OpenCameraDependency(void *context, const howdy::native::RuntimeConfig &config,
	                          const std::string &device_path)
	    -> test_cli_internal::TestPreflightOperationResult {
		auto *production_context = static_cast<TestProductionContext *>(context);
		if (production_context == nullptr) {
			return test_cli_internal::TestPreflightOperationResult{
			    .error_message = "Camera was not initialized",
			};
		}

		auto settings        = howdy::native::LoadCaptureSettings(config.video);
		settings.device_path = device_path;
		auto &capture        = production_context->capture.emplace(settings);
		if (!capture.Open()) {
			return test_cli_internal::TestPreflightOperationResult{
			    .error_message = capture.ErrorMessage(),
			};
		}
		return test_cli_internal::TestPreflightOperationResult{.ok = true};
	}

	auto ReadCameraDependency(void *context) -> bool {
		auto *production_context = static_cast<TestProductionContext *>(context);
		if (production_context == nullptr || !production_context->capture.has_value()) {
			return false;
		}
		cv::Mat frame;
		return production_context->capture->Read(frame, &production_context->prefetched_gray_frame);
	}

	auto ReadPreviewGrayFrameDependency(void *context, cv::Mat &gray_frame) -> bool {
		auto *production_context = static_cast<TestProductionContext *>(context);
		if (production_context == nullptr || !production_context->capture.has_value()) {
			return false;
		}
		cv::Mat frame;
		return production_context->capture->Read(frame, &gray_frame);
	}

	void RestorePreviewExposureDependency(void *context) {
		auto *production_context = static_cast<TestProductionContext *>(context);
		if (production_context == nullptr || !production_context->capture.has_value()) {
			return;
		}
		(void)production_context->capture->Set(cv::CAP_PROP_AUTO_EXPOSURE, 1.0);
		(void)production_context->capture->Set(cv::CAP_PROP_EXPOSURE,
		                                       static_cast<double>(production_context->exposure));
	}

	auto SwitchGuiUserDependency(void *context) -> bool {
		(void)context;
		return DropToInvokingGuiUser();
	}

	void InitializeGuiDependency(void *context) {
		auto *production_context = static_cast<TestProductionContext *>(context);
		if (production_context != nullptr && production_context->renderer.has_value()) {
			production_context->renderer->Initialize();
		}
	}

	auto PresentPreviewFrameDependency(void                                           *context,
	                                   const howdy::native::PreviewFrameResult        &frame_result,
	                                   const test_cli_internal::TestPreviewFrameStats &stats)
	    -> bool {
		return static_cast<test_cli_internal::TestPreviewRenderer *>(context)->Present(frame_result,
		                                                                               stats);
	}

	auto PreviewSlowModeDependency(void *context) -> bool {
		return static_cast<test_cli_internal::TestPreviewRenderer *>(context)->SlowMode();
	}

	auto PreviewNowDependency(void *context) -> std::chrono::steady_clock::time_point {
		(void)context;
		return std::chrono::steady_clock::now();
	}

	void PreviewSleepDependency(void *context, std::chrono::milliseconds duration) {
		(void)context;
		std::this_thread::sleep_for(duration);
	}

	auto RunPreviewDependency(void *context, const howdy::native::RuntimeConfig &config,
	                          const std::string &user, const std::string &device_path)
	    -> test_cli_internal::TestPreviewResult {
		auto *production_context = static_cast<TestProductionContext *>(context);
		if (production_context == nullptr) {
			return test_cli_internal::TestPreviewResult{
			    .status        = test_cli_internal::TestPreviewStatus::kFaceModelError,
			    .error_message = howdy::native::kFaceModelNotInitializedMessage,
			};
		}
		test_cli_internal::PreviewCleanup cleanup{production_context->renderer,
		                                          production_context->capture};

		PrepareInvokingGuiEnvironment();

		auto preflight_result = test_cli_internal::RunPreviewPreflight(
		    config, user,
		    test_cli_internal::TestPreviewPreflightDependencies{
		        .context               = production_context,
		        .face_model_ready      = FaceModelReadyDependency,
		        .has_graphical_display = HasGraphicalDisplayDependency,
		        .open_camera           = OpenCameraDependency,
		        .read_camera           = ReadCameraDependency,
		        .switch_gui_user       = SwitchGuiUserDependency,
		        .initialize_gui        = InitializeGuiDependency,
		    },
		    device_path);
		if (preflight_result.status != test_cli_internal::TestPreviewStatus::kOk) {
			return preflight_result;
		}
		if (!production_context->face_model.has_value() ||
		    !production_context->capture.has_value() || !production_context->renderer.has_value()) {
			return {
			    .status        = test_cli_internal::TestPreviewStatus::kFaceModelError,
			    .error_message = "Test preview was not initialized",
			};
		}

		production_context->exposure               = config.video.exposure;
		auto                        &face_model    = *production_context->face_model;
		auto                        &loaded_models = production_context->loaded_models;
		howdy::native::PreviewEngine preview_engine(
		    config.video,
		    {
		        .context       = &face_model,
		        .prepare_frame = PreparePreviewFrame,
		        .detect_faces  = DetectPreviewFaces,
		        .encode_face   = EncodePreviewFace,
		        .match_face    = MatchPreviewFace,
		    },
		    loaded_models.stored.encodings, loaded_models.stored.models.size(),
		    loaded_models.status == howdy::native::UserModelStatus::kOk);

		test_cli_internal::TestPreviewSession preview_session(
		    config.video, preview_engine,
		    {
		        .capture_context  = production_context,
		        .read_gray_frame  = ReadPreviewGrayFrameDependency,
		        .restore_exposure = RestorePreviewExposureDependency,
		        .renderer_context = &*production_context->renderer,
		        .present          = PresentPreviewFrameDependency,
		        .slow_mode        = PreviewSlowModeDependency,
		        .clock_context    = nullptr,
		        .now              = PreviewNowDependency,
		        .sleep_context    = nullptr,
		        .sleep            = PreviewSleepDependency,
		    });
		return test_cli_internal::RunPreviewSessionWithRetainedFrame(
		    preview_session, production_context->prefetched_gray_frame);
	}

}  // namespace

auto howdy::native::test_cli_internal::RunPreviewPreflight(
    const howdy::native::RuntimeConfig &config, const std::string &user,
    const TestPreviewPreflightDependencies &dependencies, const std::string &device_path)
    -> TestPreviewResult {
	if (dependencies.face_model_ready == nullptr || dependencies.has_graphical_display == nullptr ||
	    dependencies.open_camera == nullptr || dependencies.read_camera == nullptr ||
	    dependencies.switch_gui_user == nullptr || dependencies.initialize_gui == nullptr) {
		return TestPreviewResult{
		    .status        = TestPreviewStatus::kFaceModelError,
		    .error_message = "Internal error: missing test preview preflight dependency",
		};
	}

	auto face_model_result = dependencies.face_model_ready(dependencies.context, config, user);
	if (!face_model_result.ok) {
		return TestPreviewResult{
		    .status        = TestPreviewStatus::kFaceModelError,
		    .error_message = face_model_result.error_message,
		};
	}

	auto settings = howdy::native::LoadCaptureSettings(config.video);
	if (!device_path.empty()) {
		settings.device_path = device_path;
	}

	if (!dependencies.has_graphical_display(dependencies.context)) {
		return TestPreviewResult{.status = TestPreviewStatus::kMissingGraphicalEnvironment};
	}

	auto open_result = dependencies.open_camera(dependencies.context, config, settings.device_path);
	if (!open_result.ok) {
		return TestPreviewResult{
		    .status        = TestPreviewStatus::kCameraOpenError,
		    .error_message = open_result.error_message,
		    .device_path   = settings.device_path,
		};
	}

	std::cout << "\nOpening a window with a test feed\n\n";
	std::cout << "Press Ctrl+C here or Q in preview window to quit\n";
	std::cout << "Click on the image to enable or disable slow mode\n\n";

	if (!dependencies.switch_gui_user(dependencies.context)) {
		return TestPreviewResult{.status = TestPreviewStatus::kGuiUserError};
	}

	dependencies.initialize_gui(dependencies.context);

	if (!dependencies.read_camera(dependencies.context)) {
		return TestPreviewResult{.status = TestPreviewStatus::kCameraReadError};
	}

	return TestPreviewResult{.status = TestPreviewStatus::kOk};
}

auto howdy::native::test_cli_internal::HasGraphicalDisplayEnvironment(
    std::string_view display, std::string_view wayland_display, std::string_view runtime_dir)
    -> bool {
	if (!display.empty()) {
		return true;
	}
	return !wayland_display.empty() && !runtime_dir.empty();
}

auto howdy::native::test_cli_internal::TestMainWithDependencies(
    const howdy::native::CommandInvocation &invocation, const TestDependencies &dependencies)
    -> int {
	if (dependencies.load_runtime_config == nullptr || dependencies.run_preview == nullptr) {
		return kExitCameraError;
	}

	auto config_result = dependencies.load_runtime_config(dependencies.context);
	if (config_result.status != howdy::native::RuntimeConfigLoadStatus::kOk ||
	    !config_result.config.has_value()) {
		std::cerr << config_result.error_message << "\n";
		return kExitCameraError;
	}
	const auto &config = *config_result.config;

	const auto preview_result = dependencies.run_preview(dependencies.context, config,
	                                                     invocation.resolved_user.value_or(""),
	                                                     invocation.device.value_or(""));
	switch (preview_result.status) {
		case TestPreviewStatus::kOk:
			return kTestExitOk;
		case TestPreviewStatus::kFaceModelError:
			std::cerr << preview_result.error_message << "\n";
			return kExitCameraError;
		case TestPreviewStatus::kMissingGraphicalEnvironment:
			PrintMissingGraphicalEnvironmentDiagnostic();
			return kExitCameraError;
		case TestPreviewStatus::kCameraOpenError:
			if (preview_result.device_path == howdy::native::kNoCaptureDevice) {
				std::cerr << preview_result.error_message << "\n";
			} else {
				std::cerr << "Failed to open camera device: " << preview_result.device_path << "\n";
				std::cerr << "Error: " << preview_result.error_message << "\n";
			}
			return kExitCameraError;
		case TestPreviewStatus::kCameraReadError:
			std::cerr << howdy::native::kCameraReadFailureMessage << '\n';
			return kExitCameraError;
		case TestPreviewStatus::kGuiUserError:
			std::cerr << "Failed to switch GUI session to the invoking user\n";
			std::cerr << "Run this command from your desktop session through sudo/doas/pkexec\n";
			return kExitCameraError;
		default:
			std::cerr << "Internal error: unknown test preview status\n";
			return kExitCameraError;
	}
}

auto TestMain(const howdy::native::CommandInvocation &invocation) -> int {
	TestProductionContext production_context;
	return howdy::native::test_cli_internal::TestMainWithDependencies(
	    invocation, howdy::native::test_cli_internal::TestDependencies{
	                    .context             = &production_context,
	                    .load_runtime_config = TestCliLoadRuntimeConfigDependency,
	                    .run_preview         = RunPreviewDependency,
	                });
}
