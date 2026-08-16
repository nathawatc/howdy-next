#include "cli/test_cli.hpp"
#include "cli/test_cli_internal.hpp"
#include "cli/test_preview_renderer.hpp"
#include "cli/test_preview_session.hpp"
#include "config/runtime_config.hpp"
#include "storage/user_models.hpp"
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

namespace {

	constexpr int kExitOk          = 0;
	constexpr int kExitCameraError = 1;

	namespace test_cli_internal = howdy::native::test_cli_internal;

	struct TestArgs {
		std::string user;
		std::string device_path;
		bool        missing_device_path = false;
		bool        invalid_arguments   = false;
	};

	struct TestProductionContext {
		std::optional<howdy::native::FaceModel>               face_model;
		std::optional<howdy::native::VideoCapture>            capture;
		std::optional<test_cli_internal::TestPreviewRenderer> renderer;
		howdy::native::UserModelLoadResult                    loaded_models;
		cv::Mat                                               prefetched_gray_frame;
		int                                                   exposure = -1;
	};

	struct PreviewCleanup {
		std::optional<howdy::native::VideoCapture>   &capture;
		test_cli_internal::TestPreviewRendererCleanup renderer_cleanup;

		PreviewCleanup(std::optional<test_cli_internal::TestPreviewRenderer> &renderer,
		               std::optional<howdy::native::VideoCapture>            &preview_capture)
		    : capture(preview_capture)
		    , renderer_cleanup(renderer) {}

		~PreviewCleanup() noexcept {
			try {
				renderer_cleanup.cleanup();
			} catch (...) {  // NOLINT(bugprone-empty-catch)
			}

			try {
				if (capture.has_value()) {
					capture->release();
				}
			} catch (...) {  // NOLINT(bugprone-empty-catch)
			}
		}
	};

	auto parse_args(int argc, char **argv) -> TestArgs {
		TestArgs args;
		bool     options_ended   = false;
		bool     device_provided = false;

		for (int index = 1; index < argc; ++index) {
			const std::string_view arg(argv[index]);
			if (!options_ended && arg == "--") {
				options_ended = true;
				continue;
			}
			if (!options_ended && arg == "--device") {
				if (device_provided) {
					args.invalid_arguments = true;
					continue;
				}
				if (index + 1 >= argc || std::string_view(argv[index + 1]).empty() ||
				    std::string_view(argv[index + 1]).front() == '-') {
					args.missing_device_path = true;
					continue;
				}
				args.device_path = argv[++index];
				device_provided  = true;
				continue;
			}
			if (!options_ended && !arg.empty() && arg.front() == '-') {
				args.invalid_arguments = true;
				continue;
			}
			if (args.user.empty()) {
				args.user = arg;
			} else {
				args.invalid_arguments = true;
			}
		}

		return args;
	}

	auto prepare_preview_frame(void *context, const cv::Mat &frame) -> cv::Mat {
		(void)context;
		return howdy::native::FaceModel::prepare_frame(frame);
	}

	auto detect_preview_faces(void *context, const cv::Mat &frame)
	    -> howdy::native::FaceDetectionResult {
		return static_cast<howdy::native::FaceModel *>(context)->detect(frame);
	}

	auto encode_preview_face(void *context, const cv::Mat &frame,
	                         const howdy::native::FaceDetection &face)
	    -> howdy::native::FaceEncodingResult {
		return static_cast<howdy::native::FaceModel *>(context)->encode(frame, face);
	}

	auto match_preview_face(void *context, const std::vector<std::vector<float>> &known,
	                        const std::vector<float> &probe) -> howdy::native::FaceMatch {
		return static_cast<howdy::native::FaceModel *>(context)->best_match(known, probe);
	}

	auto drop_to_invoking_gui_user() -> bool {
		if (geteuid() != 0) {
			return true;
		}

		const auto invoking_user = howdy::native::resolve_invoking_user();
		if (!invoking_user.has_value()) {
			return false;
		}

		howdy::native::reset_invoking_user_gui_environment(*invoking_user);
		return initgroups(invoking_user->name.c_str(), invoking_user->gid) == 0 &&
		       setgid(invoking_user->gid) == 0 && setuid(invoking_user->uid) == 0;
	}

	auto getenv_string_view(const char *name) -> std::string_view {
		const char *value = std::getenv(name);
		if (value == nullptr) {
			return {};
		}
		return value;
	}

	auto has_graphical_display_environment() -> bool {
		return test_cli_internal::has_graphical_display_environment(
		    getenv_string_view("DISPLAY"), getenv_string_view("WAYLAND_DISPLAY"),
		    getenv_string_view("XDG_RUNTIME_DIR"));
	}

	void print_missing_graphical_environment_diagnostic() {
		std::cerr << "Cannot open the interactive test preview because no graphical display "
		             "environment is available.\n";
		std::cerr << "The preview needs display/session variables from your graphical login "
		             "session.\n\n";
		std::cerr << "If using sudo, preserve only the display variables, for example:\n";
		std::cerr << "  sudo "
		             "--preserve-env=DISPLAY,XAUTHORITY,WAYLAND_DISPLAY,XDG_RUNTIME_DIR "
		             "howdy test\n\n";
		std::cerr << "If using run0, pass the display variables explicitly, for example:\n";
		std::cerr << "  run0 --setenv=DISPLAY=\"$DISPLAY\" "
		             "--setenv=XAUTHORITY=\"$XAUTHORITY\" "
		             "--setenv=WAYLAND_DISPLAY=\"$WAYLAND_DISPLAY\" "
		             "--setenv=XDG_RUNTIME_DIR=\"$XDG_RUNTIME_DIR\" howdy test\n\n";
		std::cerr << "For headless testing, use:\n";
		std::cerr << "  sudo howdy snapshot\n\n";
		std::cerr << "Note: preserving these variables may still fail if your session's "
		             "display access controls deny root access.\n";
	}

	auto load_runtime_config_dependency(void *context) -> howdy::native::RuntimeConfigLoadResult {
		(void)context;
		return howdy::native::load_runtime_config();
	}

	auto face_model_ready_dependency(void *context, const howdy::native::RuntimeConfig &config,
	                                 const std::string &user)
	    -> test_cli_internal::TestPreflightOperationResult {
		auto *production_context = static_cast<TestProductionContext *>(context);
		if (production_context == nullptr) {
			return test_cli_internal::TestPreflightOperationResult{
			    .error_message = "Face model was not initialized",
			};
		}

		auto &face_model = production_context->face_model.emplace(config.face);
		if (!face_model.ok()) {
			return test_cli_internal::TestPreflightOperationResult{
			    .error_message = face_model.error_message(),
			};
		}

		production_context->loaded_models = howdy::native::UserModelLoadResult{};
		if (!user.empty()) {
			production_context->loaded_models =
			    howdy::native::load_user_models(user, howdy::native::FaceModel::kBackendName);
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

		test_cli_internal::replace_test_preview_renderer(
		    production_context->renderer, config.video,
		    production_context->loaded_models.stored.models);
		return test_cli_internal::TestPreflightOperationResult{.ok = true};
	}

	auto has_graphical_display_dependency(void *context) -> bool {
		(void)context;
		return has_graphical_display_environment();
	}

	auto open_camera_dependency(void *context, const howdy::native::RuntimeConfig &config,
	                            const std::string &device_path)
	    -> test_cli_internal::TestPreflightOperationResult {
		auto *production_context = static_cast<TestProductionContext *>(context);
		if (production_context == nullptr) {
			return test_cli_internal::TestPreflightOperationResult{
			    .error_message = "Camera was not initialized",
			};
		}

		auto settings        = howdy::native::load_capture_settings(config.video);
		settings.device_path = device_path;
		auto &capture        = production_context->capture.emplace(settings);
		if (!capture.open()) {
			return test_cli_internal::TestPreflightOperationResult{
			    .error_message = capture.error_message(),
			};
		}
		return test_cli_internal::TestPreflightOperationResult{.ok = true};
	}

	auto read_camera_dependency(void *context) -> bool {
		auto *production_context = static_cast<TestProductionContext *>(context);
		if (production_context == nullptr || !production_context->capture.has_value()) {
			return false;
		}
		cv::Mat frame;
		return production_context->capture->read(frame, &production_context->prefetched_gray_frame);
	}

	auto read_preview_gray_frame_dependency(void *context, cv::Mat &gray_frame) -> bool {
		auto *production_context = static_cast<TestProductionContext *>(context);
		if (production_context == nullptr || !production_context->capture.has_value()) {
			return false;
		}
		cv::Mat frame;
		return production_context->capture->read(frame, &gray_frame);
	}

	void restore_preview_exposure_dependency(void *context) {
		auto *production_context = static_cast<TestProductionContext *>(context);
		if (production_context == nullptr || !production_context->capture.has_value()) {
			return;
		}
		(void)production_context->capture->set(cv::CAP_PROP_AUTO_EXPOSURE, 1.0);
		(void)production_context->capture->set(cv::CAP_PROP_EXPOSURE,
		                                       static_cast<double>(production_context->exposure));
	}

	auto switch_gui_user_dependency(void *context) -> bool {
		(void)context;
		return drop_to_invoking_gui_user();
	}

	void initialize_gui_dependency(void *context) {
		auto *production_context = static_cast<TestProductionContext *>(context);
		if (production_context != nullptr && production_context->renderer.has_value()) {
			production_context->renderer->initialize();
		}
	}

	auto present_preview_frame_dependency(void                                    *context,
	                                      const howdy::native::PreviewFrameResult &frame_result,
	                                      const test_cli_internal::TestPreviewFrameStats &stats)
	    -> bool {
		return static_cast<test_cli_internal::TestPreviewRenderer *>(context)->present(frame_result,
		                                                                               stats);
	}

	auto preview_slow_mode_dependency(void *context) -> bool {
		return static_cast<test_cli_internal::TestPreviewRenderer *>(context)->slow_mode();
	}

	auto preview_now_dependency(void *context) -> std::chrono::steady_clock::time_point {
		(void)context;
		return std::chrono::steady_clock::now();
	}

	void preview_sleep_dependency(void *context, std::chrono::milliseconds duration) {
		(void)context;
		std::this_thread::sleep_for(duration);
	}

	auto run_preview_dependency(void *context, const howdy::native::RuntimeConfig &config,
	                            const std::string &user, const std::string &device_path)
	    -> test_cli_internal::TestPreviewResult {
		auto *production_context = static_cast<TestProductionContext *>(context);
		if (production_context == nullptr) {
			return test_cli_internal::TestPreviewResult{
			    .status        = test_cli_internal::TestPreviewStatus::kFaceModelError,
			    .error_message = "Face model was not initialized",
			};
		}
		PreviewCleanup cleanup{production_context->renderer, production_context->capture};

		auto preflight_result = test_cli_internal::run_preview_preflight(
		    config, user,
		    test_cli_internal::TestPreviewPreflightDependencies{
		        .context               = production_context,
		        .face_model_ready      = face_model_ready_dependency,
		        .has_graphical_display = has_graphical_display_dependency,
		        .open_camera           = open_camera_dependency,
		        .read_camera           = read_camera_dependency,
		        .switch_gui_user       = switch_gui_user_dependency,
		        .initialize_gui        = initialize_gui_dependency,
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
		        .prepare_frame = prepare_preview_frame,
		        .detect_faces  = detect_preview_faces,
		        .encode_face   = encode_preview_face,
		        .match_face    = match_preview_face,
		    },
		    loaded_models.stored.encodings, loaded_models.stored.models.size(),
		    loaded_models.status == howdy::native::UserModelStatus::kOk);

		test_cli_internal::TestPreviewSession preview_session(
		    config.video, preview_engine,
		    {
		        .capture_context  = production_context,
		        .read_gray_frame  = read_preview_gray_frame_dependency,
		        .restore_exposure = restore_preview_exposure_dependency,
		        .renderer_context = &*production_context->renderer,
		        .present          = present_preview_frame_dependency,
		        .slow_mode        = preview_slow_mode_dependency,
		        .clock_context    = nullptr,
		        .now              = preview_now_dependency,
		        .sleep_context    = nullptr,
		        .sleep            = preview_sleep_dependency,
		    });
		return test_cli_internal::run_preview_session_with_retained_frame(
		    preview_session, production_context->prefetched_gray_frame);
	}

}  // namespace

void howdy::native::test_cli_internal::run_with_preview_cleanup(
    std::optional<TestPreviewRenderer> &renderer, void *context, PreviewCleanupBodyFn body) {
	std::optional<howdy::native::VideoCapture> capture;
	PreviewCleanup                             cleanup(renderer, capture);
	body(context);
}

auto howdy::native::test_cli_internal::run_preview_preflight(
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

	auto settings = howdy::native::load_capture_settings(config.video);
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

auto howdy::native::test_cli_internal::has_graphical_display_environment(
    std::string_view display, std::string_view wayland_display, std::string_view runtime_dir)
    -> bool {
	if (!display.empty()) {
		return true;
	}
	return !wayland_display.empty() && !runtime_dir.empty();
}

auto howdy::native::test_cli_internal::test_main_with_dependencies(
    int argc, char **argv, const TestDependencies &dependencies) -> int {
	if (dependencies.load_runtime_config == nullptr || dependencies.run_preview == nullptr) {
		return kExitCameraError;
	}

	const TestArgs args = parse_args(argc, argv);
	if (args.missing_device_path) {
		std::cerr << "Error: --device requires a non-empty value\n";
		return kExitCameraError;
	}
	if (args.invalid_arguments) {
		std::cerr << "Error: invalid test arguments\n";
		return kExitCameraError;
	}

	auto config_result = dependencies.load_runtime_config(dependencies.context);
	if (config_result.status != howdy::native::RuntimeConfigLoadStatus::kOk ||
	    !config_result.config.has_value()) {
		std::cerr << config_result.error_message << "\n";
		return kExitCameraError;
	}
	const auto &config = *config_result.config;

	const auto preview_result =
	    dependencies.run_preview(dependencies.context, config, args.user, args.device_path);
	switch (preview_result.status) {
		case TestPreviewStatus::kOk:
			return kExitOk;
		case TestPreviewStatus::kFaceModelError:
			std::cerr << preview_result.error_message << "\n";
			return kExitCameraError;
		case TestPreviewStatus::kMissingGraphicalEnvironment:
			print_missing_graphical_environment_diagnostic();
			return kExitCameraError;
		case TestPreviewStatus::kCameraOpenError:
			std::cerr << "Failed to open camera device: " << preview_result.device_path << "\n";
			std::cerr << "Error: " << preview_result.error_message << "\n";
			return kExitCameraError;
		case TestPreviewStatus::kCameraReadError:
			std::cerr << "Could not capture a camera frame\n";
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

auto test_main(int argc, char **argv) -> int {
	TestProductionContext production_context;
	return howdy::native::test_cli_internal::test_main_with_dependencies(
	    argc, argv,
	    howdy::native::test_cli_internal::TestDependencies{
	        .context             = &production_context,
	        .load_runtime_config = load_runtime_config_dependency,
	        .run_preview         = run_preview_dependency,
	    });
}
