#include "cli/test_cli.hpp"
#include "common/frame_processing.hpp"
#include "common/invoking_user.hpp"
#include "common/invoking_user_env.hpp"
#include "config/runtime_config.hpp"
#include "core/face_model.hpp"
#include "recorders/video_capture.hpp"
#include "storage/user_models.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <grp.h>
#include <iostream>
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

	struct TestArgs {
		std::string user;
		std::string device_path;
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

	auto has_graphical_display_environment() -> bool {
		const char *display = std::getenv("DISPLAY");
		if (display != nullptr && display[0] != '\0') {
			return true;
		}

		const char *wayland_display = std::getenv("WAYLAND_DISPLAY");
		const char *runtime_dir     = std::getenv("XDG_RUNTIME_DIR");
		return wayland_display != nullptr && wayland_display[0] != '\0' && runtime_dir != nullptr &&
		       runtime_dir[0] != '\0';
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

}  // namespace

int test_main(int argc, char **argv) {
	const TestArgs args          = parse_args(argc, argv);
	auto           config_result = howdy::native::load_runtime_config();
	if (config_result.status != howdy::native::RuntimeConfigLoadStatus::kOk ||
	    !config_result.config.has_value()) {
		std::cerr << config_result.error_message << "\n";
		return kExitCameraError;
	}
	const auto &config = *config_result.config;

	howdy::native::FaceModel face_model(config.face);
	if (!face_model.ok()) {
		std::cerr << face_model.error_message() << "\n";
		return kExitCameraError;
	}

	auto loaded_models = howdy::native::UserModelLoadResult{};
	if (!args.user.empty()) {
		loaded_models =
		    howdy::native::load_user_models(args.user, howdy::native::FaceModel::kBackendName);
		if (loaded_models.status == howdy::native::UserModelStatus::kIncompatibleBackend) {
			std::cout
			    << "Warning: Stored face models use an incompatible backend; matching disabled\n";
		} else if (loaded_models.status == howdy::native::UserModelStatus::kInvalidUser) {
			std::cout << "Warning: Invalid user name; matching disabled\n";
		} else if (loaded_models.status == howdy::native::UserModelStatus::kNoModel) {
			std::cout << "Warning: No face model found for this user, detection will run without "
			             "matching\n";
		} else if (loaded_models.status == howdy::native::UserModelStatus::kParseError) {
			std::cout << "Warning: Failed to read stored face models, detection will run without "
			             "matching\n";
		} else if (loaded_models.status == howdy::native::UserModelStatus::kInsecurePath) {
			std::cout << "Warning: Stored face model path is insecure, detection will run without "
			             "matching\n";
		}
	}

	auto settings = howdy::native::load_capture_settings(config.video);
	if (!args.device_path.empty()) {
		settings.device_path = args.device_path;
	}

	if (!has_graphical_display_environment()) {
		print_missing_graphical_environment_diagnostic();
		return kExitCameraError;
	}

	howdy::native::VideoCapture capture(settings);
	if (!capture.open()) {
		std::cerr << "Failed to open camera device: " << settings.device_path << "\n";
		std::cerr << "Error: " << capture.error_message() << "\n";
		return kExitCameraError;
	}

	const int   exposure       = config.video.exposure;
	const float dark_threshold = config.video.dark_threshold;
	auto        clahe          = howdy::native::make_clahe(config.video);

	std::cout << "\nOpening a window with a test feed\n\n";
	std::cout << "Press ctrl+C in this terminal to quit\n";
	std::cout << "Click on the image to enable or disable slow mode\n\n";

	if (!drop_to_invoking_gui_user()) {
		std::cerr << "Failed to switch GUI session to the invoking user\n";
		std::cerr << "Run this command from your desktop session through sudo/doas/pkexec\n";
		capture.release();
		return kExitCameraError;
	}

	cv::namedWindow(kWindowName);
	cv::setMouseCallback(kWindowName, mouse_callback);

	int    total_frames   = 0;
	int    sec_frames     = 0;
	int    fps            = 0;
	auto   second_anchor  = std::chrono::steady_clock::now();
	double recognition_ms = 0.0;

	try {
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
			if (!capture.read(frame, &gray_frame)) {
				std::cerr << "Failed to read frame from camera\n";
				capture.release();
				return kExitCameraError;
			}

			howdy::native::apply_clahe_if_enabled(gray_frame, config.video, clahe);

			cv::Mat overlay;
			cv::cvtColor(gray_frame.clone(), overlay, cv::COLOR_GRAY2BGR);
			const int height = gray_frame.rows;
			const int width  = gray_frame.cols;

			const auto brightness = howdy::native::measure_brightness(gray_frame);
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
			           "RECOGNITION: " + std::to_string(static_cast<int>(recognition_ms)) + "ms");
			print_text(overlay, 4, height, "BACKEND: OpenCV YuNet/SFace");
			print_text(overlay, 5, height,
			           std::string("CLAHE: ") + (config.video.clahe_enabled ? "on" : "off"));

			if (g_slow_mode) {
				cv::putText(overlay, "SLOW MODE", cv::Point(width - 66, height - 10),
				            cv::FONT_HERSHEY_SIMPLEX, 0.3, cv::Scalar(0, 0, 255), 0, cv::LINE_AA);
			}

			if (brightness.darkness > dark_threshold) {
				cv::putText(overlay, "DARK FRAME", cv::Point(width - 68, 16),
				            cv::FONT_HERSHEY_SIMPLEX, 0.3, cv::Scalar(0, 0, 255), 0, cv::LINE_AA);
			} else {
				cv::putText(overlay, "SCAN FRAME", cv::Point(width - 68, 16),
				            cv::FONT_HERSHEY_SIMPLEX, 0.3, cv::Scalar(0, 255, 0), 0, cv::LINE_AA);

				const auto recognition_start = std::chrono::steady_clock::now();
				auto       face_frame        = face_model.prepare_frame(gray_frame);
				const auto face_locations    = face_model.detect(face_frame);
				recognition_ms =
				    static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(
				                            std::chrono::steady_clock::now() - recognition_start)
				                            .count());

				for (const auto &face : face_locations) {
					cv::Scalar color(0, 0, 230);
					const auto [x, y, w, h] = face_model.detection_box(face);
					const float confidence  = face_model.detection_confidence(face);

					if (loaded_models.status == howdy::native::UserModelStatus::kOk) {
						const auto face_encoding = face_model.encode(face_frame, face);
						const auto match =
						    face_model.best_match(loaded_models.stored.encodings, face_encoding);

						std::string face_text;
						if (match.accepted) {
							color = cv::Scalar(0, 230, 0);
							const auto &model =
							    loaded_models.stored.models[static_cast<std::size_t>(match.index)];
							face_text =
							    model.label + " (score: " + cv::format("%.3f", match.score) + ")";
						} else {
							face_text = "no match (" + cv::format("%.3f", match.score) + ")";
						}

						cv::putText(overlay, face_text, cv::Point(x, std::max(0, y - 8)),
						            cv::FONT_HERSHEY_SIMPLEX, 0.3, color, 0, cv::LINE_AA);
					}

					cv::rectangle(overlay, cv::Rect(x, y, w, h), color, 2);
					cv::putText(overlay, cv::format("%.2f", confidence),
					            cv::Point(x, std::min(height - 4, y + h + 12)),
					            cv::FONT_HERSHEY_SIMPLEX, 0.3, color, 0, cv::LINE_AA);
					for (const auto &point : face_model.detection_landmarks(face)) {
						cv::circle(overlay, point, 2, cv::Scalar(0, 255, 255), -1);
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
	} catch (...) {
		cv::destroyAllWindows();
		capture.release();
		throw;
	}

	cv::destroyAllWindows();
	capture.release();
	return kExitOk;
}
