#include "cli/test_cli.hpp"
#include "cli/test_cli_internal.hpp"
#include "common/invoking_user.hpp"
#include "common/invoking_user_env.hpp"
#include "common/preview_engine.hpp"
#include "config/runtime_config.hpp"
#include "core/face_model.hpp"
#include "recorders/video_capture.hpp"
#include "storage/user_models.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <grp.h>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>

#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>

namespace {

	constexpr int  kExitOk          = 0;
	constexpr int  kExitCameraError = 1;
	constexpr auto kWindowName      = "Howdy Test";

	namespace test_cli_internal = howdy::native::test_cli_internal;

	struct TestArgs {
		std::string user;
		std::string device_path;
	};

	struct TestProductionContext {
		std::optional<howdy::native::FaceModel>    face_model;
		std::optional<howdy::native::VideoCapture> capture;
		howdy::native::UserModelLoadResult         loaded_models;
		cv::Mat                                    prefetched_frame;
		cv::Mat                                    prefetched_gray_frame;
		bool                                       gui_initialized = false;
	};

	struct PreviewCleanup {
		TestProductionContext &context;

		~PreviewCleanup() noexcept {
			try {
				if (context.gui_initialized) {
					cv::destroyAllWindows();
				}
			} catch (...) {  // NOLINT(bugprone-empty-catch)
			}

			try {
				if (context.capture.has_value()) {
					context.capture->release();
				}
			} catch (...) {  // NOLINT(bugprone-empty-catch)
			}
		}
	};

	bool g_slow_mode = false;

	void mouse_callback(int event, int x, int y, int flags, void *userdata) {
		(void)x;
		(void)y;
		(void)flags;
		(void)userdata;
		if (event == cv::EVENT_LBUTTONDOWN) {
			g_slow_mode = !g_slow_mode;
		}
	}

	auto parse_args(int argc, char **argv) -> TestArgs {
		TestArgs args;

		for (int index = 1; index < argc; ++index) {
			const std::string_view arg(argv[index]);
			if (arg == "--device" && index + 1 < argc) {
				args.device_path = argv[++index];
				continue;
			}
			if (!arg.empty() && arg.front() != '-' && args.user.empty()) {
				args.user = argv[index];
			}
		}

		return args;
	}

	void print_text(cv::Mat &overlay, int line_number, int height, const std::string &text) {
		cv::putText(overlay, text, cv::Point(10, height - 10 - (10 * line_number)),
		            cv::FONT_HERSHEY_SIMPLEX, 0.3, cv::Scalar(0, 255, 0), 0, cv::LINE_AA);
	}

	auto prepare_preview_frame(void *context, const cv::Mat &frame) -> cv::Mat {
		return static_cast<howdy::native::FaceModel *>(context)->prepare_frame(frame);
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
		if (!production_context->capture->read(production_context->prefetched_frame,
		                                       &production_context->prefetched_gray_frame)) {
			return false;
		}
		return true;
	}

	auto switch_gui_user_dependency(void *context) -> bool {
		(void)context;
		return drop_to_invoking_gui_user();
	}

	void initialize_gui_dependency(void *context) {
		auto *production_context = static_cast<TestProductionContext *>(context);
		if (production_context != nullptr) {
			production_context->gui_initialized = true;
		}
		cv::namedWindow(kWindowName);
		cv::setMouseCallback(kWindowName, mouse_callback);
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
		PreviewCleanup cleanup{*production_context};

		auto preflight_result = test_cli_internal::run_preview_preflight(
		    config, user, device_path,
		    test_cli_internal::TestPreviewPreflightDependencies{
		        .context               = production_context,
		        .face_model_ready      = face_model_ready_dependency,
		        .has_graphical_display = has_graphical_display_dependency,
		        .open_camera           = open_camera_dependency,
		        .read_camera           = read_camera_dependency,
		        .switch_gui_user       = switch_gui_user_dependency,
		        .initialize_gui        = initialize_gui_dependency,
		    });
		if (preflight_result.status != test_cli_internal::TestPreviewStatus::kOk) {
			return preflight_result;
		}

		auto                        &face_model     = *production_context->face_model;
		auto                        &loaded_models  = production_context->loaded_models;
		auto                        &capture        = *production_context->capture;
		const int                    exposure       = config.video.exposure;
		bool                         has_prefetched = true;
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

		int                       total_frames  = 0;
		int                       sec_frames    = 0;
		int                       fps           = 0;
		auto                      second_anchor = std::chrono::steady_clock::now();
		std::chrono::milliseconds inference_time{0};

		while (true) {
			const auto frame_start = std::chrono::steady_clock::now();
			total_frames++;
			sec_frames++;

			if (std::chrono::duration_cast<std::chrono::seconds>(frame_start - second_anchor)
			        .count() >= 1) {
				fps           = sec_frames;
				sec_frames    = 0;
				second_anchor = frame_start;
			}

			cv::Mat frame;
			cv::Mat gray_frame;
			if (has_prefetched) {
				frame          = production_context->prefetched_frame;
				gray_frame     = production_context->prefetched_gray_frame;
				has_prefetched = false;
			} else if (!capture.read(frame, &gray_frame)) {
				return test_cli_internal::TestPreviewResult{
				    .status = test_cli_internal::TestPreviewStatus::kCameraReadError,
				};
			}

			auto frame_result        = preview_engine.process_gray_frame(std::move(gray_frame));
			inference_time           = frame_result.inference_time;
			const auto frame_failure = test_cli_internal::map_preview_frame_failure(frame_result);
			if (frame_failure.status != test_cli_internal::TestPreviewStatus::kOk) {
				return frame_failure;
			}
			gray_frame = frame_result.gray_frame;

			cv::Mat overlay;
			cv::cvtColor(gray_frame.clone(), overlay, cv::COLOR_GRAY2BGR);
			const int height = gray_frame.rows;
			const int width  = gray_frame.cols;

			const auto &brightness = frame_result.brightness;
			for (std::size_t index = 0; index < brightness.bins_percent.size(); ++index) {
				const float     value_perc = brightness.bins_percent[index];
				const int       bin_offset = 10 * static_cast<int>(index);
				const cv::Point p1(20 + bin_offset, 10);
				const cv::Point p2(10 + bin_offset, static_cast<int>((value_perc / 2.0F) + 10.0F));
				cv::rectangle(overlay, p1, p2, cv::Scalar(0, 200, 0), cv::FILLED);
			}

			print_text(overlay, 0, height,
			           "RESOLUTION: " + std::to_string(height) + "x" + std::to_string(width));
			print_text(overlay, 1, height, "FPS: " + std::to_string(fps));
			print_text(overlay, 2, height, "FRAMES: " + std::to_string(total_frames));
			print_text(overlay, 3, height,
			           "INFERENCE: " + std::to_string(inference_time.count()) + "ms");
			print_text(overlay, 4, height, "BACKEND: OpenCV YuNet/SFace");
			print_text(overlay, 5, height,
			           std::string("CLAHE: ") + (config.video.clahe_enabled ? "on" : "off"));

			if (g_slow_mode) {
				cv::putText(overlay, "SLOW MODE", cv::Point(width - 66, height - 10),
				            cv::FONT_HERSHEY_SIMPLEX, 0.3, cv::Scalar(0, 0, 255), 0, cv::LINE_AA);
			}

			const bool dark_frame =
			    frame_result.status == howdy::native::PreviewFrameStatus::kBlackFrame ||
			    frame_result.status == howdy::native::PreviewFrameStatus::kTooDark;
			if (dark_frame) {
				cv::putText(overlay, "DARK FRAME", cv::Point(width - 68, 16),
				            cv::FONT_HERSHEY_SIMPLEX, 0.3, cv::Scalar(0, 0, 255), 0, cv::LINE_AA);
			} else {
				cv::putText(overlay, "SCAN FRAME", cv::Point(width - 68, 16),
				            cv::FONT_HERSHEY_SIMPLEX, 0.3, cv::Scalar(0, 255, 0), 0, cv::LINE_AA);
				for (const auto &face_result : frame_result.faces) {
					const auto &face = face_result.detection;
					cv::Scalar  color(0, 0, 230);
					const int   x = static_cast<int>(face.box.x);
					const int   y = static_cast<int>(face.box.y);
					const int   w = static_cast<int>(face.box.width);
					const int   h = static_cast<int>(face.box.height);

					if (face_result.status == howdy::native::PreviewFaceStatus::kEncodingFailed) {
						cv::putText(overlay, "encoding failed", cv::Point(x, std::max(0, y - 8)),
						            cv::FONT_HERSHEY_SIMPLEX, 0.3, color, 0, cv::LINE_AA);
					} else if (face_result.matching_attempted) {
						const auto &face_match = face_result.match;
						std::string face_text;
						if (!face_match.accepted) {
							face_text = "no match (" + cv::format("%.3f", face_match.score) + ")";
						} else {
							color = cv::Scalar(0, 230, 0);
							const auto &model =
							    loaded_models.stored
							        .models[static_cast<std::size_t>(face_match.index)];
							face_text = model.label +
							            " (score: " + cv::format("%.3f", face_match.score) + ")";
						}

						cv::putText(overlay, face_text, cv::Point(x, std::max(0, y - 8)),
						            cv::FONT_HERSHEY_SIMPLEX, 0.3, color, 0, cv::LINE_AA);
					}

					cv::rectangle(overlay, cv::Rect(x, y, w, h), color, 2);
					cv::putText(overlay, cv::format("%.2f", face.confidence),
					            cv::Point(x, std::min(height - 4, y + h + 12)),
					            cv::FONT_HERSHEY_SIMPLEX, 0.3, color, 0, cv::LINE_AA);
					for (const auto &point : face.landmarks) {
						cv::circle(overlay,
						           cv::Point(static_cast<int>(point.x), static_cast<int>(point.y)),
						           2, cv::Scalar(0, 255, 255), -1);
					}
				}
			}

			cv::Mat display_frame;
			cv::cvtColor(gray_frame, display_frame, cv::COLOR_GRAY2BGR);
			cv::addWeighted(overlay, 0.65, display_frame, 0.35, 0, display_frame);
			cv::imshow(kWindowName, display_frame);

			if (cv::waitKey(1) != -1) {
				break;
			}

			const auto frame_time = std::chrono::duration_cast<std::chrono::milliseconds>(
			                            std::chrono::steady_clock::now() - frame_start)
			                            .count();
			if (g_slow_mode) {
				const auto sleep_ms = std::max(0LL, 500LL - frame_time);
				std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
			}

			if (exposure != -1) {
				capture.set(cv::CAP_PROP_AUTO_EXPOSURE, 1.0);
				capture.set(cv::CAP_PROP_EXPOSURE, static_cast<double>(exposure));
			}
		}
		return test_cli_internal::TestPreviewResult{.status =
		                                                test_cli_internal::TestPreviewStatus::kOk};
	}

}  // namespace

auto howdy::native::test_cli_internal::map_preview_frame_failure(
    const howdy::native::PreviewFrameResult &result) -> TestPreviewResult {
	switch (result.status) {
		case howdy::native::PreviewFrameStatus::kInvalidFrame:
			return {
			    .status        = TestPreviewStatus::kCameraReadError,
			    .error_message = result.error_message,
			};
		case howdy::native::PreviewFrameStatus::kDetectionFailed:
		case howdy::native::PreviewFrameStatus::kEncodingFailed:
		case howdy::native::PreviewFrameStatus::kInvalidMatchResult:
		case howdy::native::PreviewFrameStatus::kInvalidDependencies:
			return {
			    .status        = TestPreviewStatus::kFaceModelError,
			    .error_message = result.error_message,
			};
		default:
			return {.status = TestPreviewStatus::kOk};
	}
}

auto howdy::native::test_cli_internal::run_preview_preflight(
    const howdy::native::RuntimeConfig &config, const std::string &user,
    const std::string &device_path, const TestPreviewPreflightDependencies &dependencies)
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
	std::cout << "Press ctrl+C in this terminal to quit\n";
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

	const TestArgs args          = parse_args(argc, argv);
	auto           config_result = dependencies.load_runtime_config(dependencies.context);
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
			std::cerr << "Failed to read frame from camera\n";
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

int test_main(int argc, char **argv) {
	TestProductionContext production_context;
	return howdy::native::test_cli_internal::test_main_with_dependencies(
	    argc, argv,
	    howdy::native::test_cli_internal::TestDependencies{
	        .context             = &production_context,
	        .load_runtime_config = load_runtime_config_dependency,
	        .run_preview         = run_preview_dependency,
	    });
}
