#include "common/compare_args.hpp"
#include "common/compare_engine.hpp"
#include "common/compare_exit.hpp"
#include "common/compare_logic.hpp"
#include "common/frame_validation.hpp"
#include "config/runtime_config.hpp"
#include "config/runtime_paths.hpp"
#include "core/face_model.hpp"
#include "recorders/video_capture.hpp"
#include "storage/user_models.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <iostream>
#include <limits>
#include <string>
#include <utility>

#include <sys/prctl.h>
#include <sys/resource.h>

namespace {

	using howdy::native::CompareExit;
	constexpr rlim_t kAddressSpaceLimitBytes =
	    static_cast<rlim_t>(3ULL * 1024ULL * 1024ULL * 1024ULL);

	auto apply_compare_sandbox(int timeout_seconds) -> bool {
		if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
			std::cerr << "Failed to enable no_new_privs sandboxing\n";
			return false;
		}

		rlimit cpu_limit{};
		cpu_limit.rlim_cur = static_cast<rlim_t>(std::max(timeout_seconds + 5, 15));
		cpu_limit.rlim_max = static_cast<rlim_t>(std::max(timeout_seconds + 10, 20));
		if (setrlimit(RLIMIT_CPU, &cpu_limit) != 0) {
			std::cerr << "Failed to apply CPU sandbox limit\n";
			return false;
		}

		rlimit file_limit{};
		file_limit.rlim_cur = 64;
		file_limit.rlim_max = 64;
		if (setrlimit(RLIMIT_NOFILE, &file_limit) != 0) {
			std::cerr << "Failed to apply file-descriptor sandbox limit\n";
			return false;
		}

		rlimit core_limit{};
		core_limit.rlim_cur = 0;
		core_limit.rlim_max = 0;
		if (setrlimit(RLIMIT_CORE, &core_limit) != 0) {
			std::cerr << "Failed to disable core dumps\n";
			return false;
		}

		rlimit address_space_limit{};
		address_space_limit.rlim_cur = kAddressSpaceLimitBytes;
		address_space_limit.rlim_max = address_space_limit.rlim_cur;
		if (setrlimit(RLIMIT_AS, &address_space_limit) != 0) {
			std::cerr << "Failed to apply memory sandbox limit\n";
			return false;
		}

		return true;
	}

	auto prepared_frame_validation_message(const cv::Mat                       &frame,
	                                       howdy::native::FrameValidationStatus status)
	    -> std::string {
		const std::string subject = "Prepared frame for face detection";
		switch (status) {
			case howdy::native::FrameValidationStatus::kValid:
				return {};
			case howdy::native::FrameValidationStatus::kEmpty:
				return subject + " is empty";
			case howdy::native::FrameValidationStatus::kUnsupportedDimensions:
				return subject + " has unsupported frame dimensions: " + std::to_string(frame.dims);
			case howdy::native::FrameValidationStatus::kOversizedDimensions:
				return subject + " has oversized frame dimensions: " + std::to_string(frame.cols) +
				       "x" + std::to_string(frame.rows) + " (max supported dimension: " +
				       std::to_string(howdy::native::kMaxFrameDimension) + ")";
			case howdy::native::FrameValidationStatus::kUnsupportedChannelCount:
				return subject +
				       " has unsupported channel count: " + std::to_string(frame.channels());
			case howdy::native::FrameValidationStatus::kUnsupportedPixelType:
				return subject + " has unsupported pixel type: " + std::to_string(frame.type());
		}
		return subject + " is invalid";
	}

}  // namespace

auto main(int argc, char **argv) -> int {
	try {
		const auto start_time   = std::chrono::steady_clock::now();
		const auto parse_result = howdy::native::parse_compare_args(
		    argc, argv, howdy::native::resolve_config_path().string());
		if (parse_result.status == howdy::native::CompareArgsStatus::kHelp) {
			std::cout << parse_result.message;
			return static_cast<int>(parse_result.exit_code);
		}
		if (parse_result.status == howdy::native::CompareArgsStatus::kError) {
			if (!parse_result.message.empty()) {
				std::cerr << parse_result.message;
			}
			return static_cast<int>(parse_result.exit_code);
		}
		const auto &args = parse_result.args;

		auto config_result =
		    howdy::native::load_runtime_config(args.config_path, static_cast<uid_t>(0));
		if (config_result.status != howdy::native::RuntimeConfigLoadStatus::kOk ||
		    !config_result.config.has_value()) {
			std::cerr << config_result.error_message << "\n";
			return static_cast<int>(CompareExit::kAbort);
		}
		const auto &config = *config_result.config;

		if (!apply_compare_sandbox(config.video.timeout)) {
			return static_cast<int>(CompareExit::kAbort);
		}

		const auto loaded_models = howdy::native::load_user_models(
		    args.user, howdy::native::FaceModel::kBackendName, static_cast<uid_t>(0));
		if (loaded_models.status == howdy::native::UserModelStatus::kInvalidUser) {
			std::cerr << loaded_models.error_message << "\n";
			return static_cast<int>(CompareExit::kAbort);
		}
		if (loaded_models.status == howdy::native::UserModelStatus::kIncompatibleBackend) {
			std::cerr << loaded_models.error_message << "\n";
			return static_cast<int>(CompareExit::kNoFaceModel);
		}
		if (loaded_models.status == howdy::native::UserModelStatus::kParseError) {
			std::cerr << loaded_models.error_message << "\n";
			return static_cast<int>(CompareExit::kAbort);
		}
		if (loaded_models.status == howdy::native::UserModelStatus::kInsecurePath) {
			std::cerr << loaded_models.error_message << "\n";
			return static_cast<int>(CompareExit::kAbort);
		}
		if (loaded_models.status != howdy::native::UserModelStatus::kOk) {
			return static_cast<int>(CompareExit::kNoFaceModel);
		}

		howdy::native::FaceModel face_model(config.face);
		if (!face_model.ok()) {
			std::cerr << face_model.error_message() << "\n";
			return static_cast<int>(CompareExit::kAbort);
		}

		howdy::native::VideoCapture capture(howdy::native::load_capture_settings(config.video));
		if (!capture.open()) {
			std::cerr << capture.error_message() << "\n";
			return static_cast<int>(CompareExit::kInvalidDevice);
		}

		const int                    timeout    = config.video.timeout;
		const int                    exposure   = config.video.exposure;
		const bool                   end_report = config.debug.end_report;
		howdy::native::CompareEngine compare_engine(config.video);

		int    frames             = 0;
		int    black_tries        = 0;
		int    dark_tries         = 0;
		int    valid_frames       = 0;
		double dark_running_total = 0.0;
		float  best_score         = std::numeric_limits<float>::quiet_NaN();
		float  winning_score      = 0.0F;
		int    winning_index      = -1;
		auto   frame_loop_start   = std::chrono::steady_clock::now();

		while (true) {
			frames++;

			const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
			                         std::chrono::steady_clock::now() - frame_loop_start)
			                         .count();
			if (elapsed > timeout) {
				const auto exit_code = howdy::native::timeout_exit(dark_tries, valid_frames);
				if (exit_code == CompareExit::kTooDark) {
					std::cerr
					    << "All frames were too dark, please check dark_threshold in config\n";
					std::cerr << "Average darkness: "
					          << (dark_running_total / std::max(valid_frames, 1))
					          << ", Threshold: " << config.video.dark_threshold << "\n";
				}
				return static_cast<int>(exit_code);
			}

			cv::Mat frame;
			cv::Mat gray_frame;
			if (!capture.read(frame, &gray_frame)) {
				std::cerr << capture.error_message() << "\n";
				return static_cast<int>(CompareExit::kInvalidDevice);
			}

			const auto frame_result =
			    compare_engine.process_gray_frame(std::move(gray_frame), frames);

			switch (frame_result.status) {
				case howdy::native::CompareFrameStatus::kBlackFrame:
					black_tries++;
					continue;

				case howdy::native::CompareFrameStatus::kTooDark:
					dark_running_total += frame_result.brightness.darkness;
					valid_frames++;
					dark_tries++;
					continue;

				case howdy::native::CompareFrameStatus::kInvalidInput:
					std::cerr << frame_result.error_message << "\n";
					return static_cast<int>(CompareExit::kInvalidDevice);

				case howdy::native::CompareFrameStatus::kInvalidPreprocessed:
					std::cerr << frame_result.error_message << "\n";
					return static_cast<int>(CompareExit::kAbort);

				case howdy::native::CompareFrameStatus::kReady:
					dark_running_total += frame_result.brightness.darkness;
					valid_frames++;
					break;
			}

			cv::Mat working_frame = frame_result.working_frame;

			cv::Mat    prepared = face_model.prepare_frame(working_frame);
			const auto prepared_validation =
			    howdy::native::validate_frame(prepared, howdy::native::FrameChannelPolicy::kBgr);
			if (prepared_validation != howdy::native::FrameValidationStatus::kValid) {
				std::cerr << prepared_frame_validation_message(prepared, prepared_validation)
				          << "\n";
				return static_cast<int>(CompareExit::kAbort);
			}
			const auto detection_result = face_model.detect(prepared);
			if (!detection_result.ok()) {
				std::cerr << detection_result.error_message << "\n";
				return static_cast<int>(CompareExit::kAbort);
			}
			for (const auto &face : detection_result.detections) {
				const auto encoding = face_model.encode(prepared, face);
				const auto match = face_model.best_match(loaded_models.stored.encodings, encoding);
				best_score =
				    howdy::native::update_best_score(best_score, match.score, face_model.metric());

				if (!match.accepted) {
					continue;
				}

				winning_index = match.index;
				winning_score = match.score;

				if (end_report) {
					const auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
					                          std::chrono::steady_clock::now() - start_time)
					                          .count();
					std::cout << "Total time: " << total_ms << "ms\n";
					std::cout << "Frames searched: " << frames << "\n";
					std::cout << "Black frames ignored: " << black_tries << "\n";
					std::cout << "Dark frames ignored: " << dark_tries << "\n";
					std::cout << "Winning score: " << winning_score << "\n";
					if (winning_index >= 0 &&
					    std::cmp_less(winning_index, loaded_models.stored.models.size())) {
						const auto &winner =
						    loaded_models.stored.models[static_cast<std::size_t>(winning_index)];
						std::cout << "Winning model: " << winner.id << " (\"" << winner.label
						          << "\")\n";
					}
				}

				return static_cast<int>(CompareExit::kSuccess);
			}

			if (exposure != -1) {
				capture.set(cv::CAP_PROP_AUTO_EXPOSURE, 1.0);
				capture.set(cv::CAP_PROP_EXPOSURE, static_cast<double>(exposure));
			}
		}
	} catch (const cv::Exception &error) {
		return static_cast<int>(howdy::native::compare_abort_from_cv_exception(
		    error, std::cerr, "authentication compare path"));
	} catch (const std::exception &error) {
		return static_cast<int>(howdy::native::compare_abort_from_exception(
		    error, std::cerr, "authentication compare path"));
	} catch (...) {
		return static_cast<int>(howdy::native::compare_abort_from_unknown_exception(
		    std::cerr, "authentication compare path"));
	}
}
