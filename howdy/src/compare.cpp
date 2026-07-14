#include "common/compare_args.hpp"
#include "common/compare_capture_session.hpp"
#include "common/compare_engine.hpp"
#include "common/compare_exit.hpp"
#include "common/compare_logic.hpp"
#include "common/compare_processing_internal.hpp"
#include "common/compare_sandbox.hpp"
#include "config/runtime_config.hpp"
#include "config/runtime_paths.hpp"
#include "core/face_model.hpp"
#include "storage/user_models.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <utility>

namespace {

	using howdy::native::CompareExit;

	auto sandbox_resource_name(howdy::native::CompareSandboxResource resource) -> const char * {
		switch (resource) {
			case howdy::native::CompareSandboxResource::kCpu:
				return "CPU";
			case howdy::native::CompareSandboxResource::kOpenFiles:
				return "file-descriptor";
			case howdy::native::CompareSandboxResource::kCore:
				return "core-dump";
			case howdy::native::CompareSandboxResource::kAddressSpace:
				return "address-space";
			case howdy::native::CompareSandboxResource::kNone:
				return "unknown";
		}
		return "unknown";
	}

	void report_sandbox_failure(const howdy::native::CompareSandboxResult &result) {
		switch (result.status) {
			case howdy::native::CompareSandboxStatus::kOk:
				return;
			case howdy::native::CompareSandboxStatus::kNoNewPrivilegesFailure:
				std::cerr << "Failed to enable no_new_privs sandboxing";
				break;
			case howdy::native::CompareSandboxStatus::kLimitInspectionFailure:
				std::cerr << "Failed to inspect " << sandbox_resource_name(result.resource)
				          << " sandbox limit";
				break;
			case howdy::native::CompareSandboxStatus::kLimitBelowMinimum:
				std::cerr << "Inherited " << sandbox_resource_name(result.resource)
				          << " limit is below Howdy's minimum sandbox policy\n";
				return;
			case howdy::native::CompareSandboxStatus::kLimitApplicationFailure:
				std::cerr << "Failed to apply " << sandbox_resource_name(result.resource)
				          << " sandbox limit";
				break;
		}
		if (result.error_number != 0) {
			std::cerr << ": " << std::strerror(result.error_number);
		}
		std::cerr << "\n";
	}

	auto prepare_face_frame_dependency(void *context, const cv::Mat &frame) -> cv::Mat {
		return static_cast<howdy::native::FaceModel *>(context)->prepare_frame(frame);
	}

	auto detect_faces_dependency(void *context, const cv::Mat &frame)
	    -> howdy::native::FaceDetectionResult {
		return static_cast<howdy::native::FaceModel *>(context)->detect(frame);
	}

	auto encode_face_dependency(void *context, const cv::Mat &frame,
	                            const howdy::native::FaceDetection &face)
	    -> howdy::native::FaceEncodingResult {
		return static_cast<howdy::native::FaceModel *>(context)->encode(frame, face);
	}

	auto find_best_match_dependency(void *context, const std::vector<std::vector<float>> &known,
	                                const std::vector<float> &probe) -> howdy::native::FaceMatch {
		return static_cast<howdy::native::FaceModel *>(context)->best_match(known, probe);
	}

	struct CompareProductionContext {
		howdy::native::CompareCaptureSession       &capture_session;
		howdy::native::FaceModel                   &face_model;
		const howdy::native::VideoConfig           &video_config;
		const howdy::native::StoredEncodings       &stored_encodings;
		std::chrono::steady_clock::time_point       start_time;
		bool                                        end_report = false;
		std::optional<howdy::native::CompareEngine> compare_engine;
	};

	auto open_capture(void *raw_context) -> howdy::native::CompareCaptureOpenResult {
		auto &context = *static_cast<CompareProductionContext *>(raw_context);
		return context.capture_session.open();
	}

	auto drop_privileges([[maybe_unused]] void *raw_context)
	    -> howdy::native::ComparePrivilegeResult {
		return howdy::native::drop_compare_privileges();
	}

	void construct_engine(void *raw_context) {
		auto &context = *static_cast<CompareProductionContext *>(raw_context);
		context.compare_engine.emplace(context.video_config,
		                               howdy::native::CompareInferenceDependencies{
		                                   .context            = &context.face_model,
		                                   .prepare_face_frame = prepare_face_frame_dependency,
		                                   .detect_faces       = detect_faces_dependency,
		                                   .encode_face        = encode_face_dependency,
		                                   .find_best_match    = find_best_match_dependency,
		                               },
		                               context.stored_encodings.encodings);
	}

	void reset_timeout(void *raw_context) {
		auto &context = *static_cast<CompareProductionContext *>(raw_context);
		context.capture_session.reset_timeout_clock();
	}

	auto run_frame_loop(void *raw_context) -> CompareExit {
		auto &context = *static_cast<CompareProductionContext *>(raw_context);
		if (!context.compare_engine.has_value()) {
			return CompareExit::kAbort;
		}
		auto &capture_session = context.capture_session;
		auto &compare_engine  = *context.compare_engine;

		float winning_score = 0.0F;
		int   winning_index = -1;

		while (true) {
			auto capture_result = capture_session.next_frame();

			switch (capture_result.status) {
				case howdy::native::CompareCaptureFrameStatus::kFrameReady:
					break;

				case howdy::native::CompareCaptureFrameStatus::kTimeout: {
					const auto &stats = capture_session.stats();
					const auto  exit_code =
					    howdy::native::timeout_exit(stats.dark_frames, stats.valid_frames);

					if (exit_code == CompareExit::kTooDark) {
						std::cerr
						    << "All frames were too dark, please check dark_threshold in config\n";
						std::cerr << "Average darkness: "
						          << (stats.dark_running_total / std::max(stats.valid_frames, 1))
						          << ", Threshold: " << context.video_config.dark_threshold << "\n";
					}

					return exit_code;
				}

				case howdy::native::CompareCaptureFrameStatus::kReadFailed:
					std::cerr << capture_result.error_message << "\n";
					return CompareExit::kInvalidDevice;

				case howdy::native::CompareCaptureFrameStatus::kNotOpen:
				case howdy::native::CompareCaptureFrameStatus::kInvalidDependencies:
					return CompareExit::kAbort;
			}

			const auto frame_result = compare_engine.process_gray_frame(
			    std::move(capture_result.gray_frame), capture_result.frame_number);

			switch (frame_result.status) {
				case howdy::native::CompareFrameStatus::kBlackFrame:
					capture_session.record_black_frame();
					continue;

				case howdy::native::CompareFrameStatus::kTooDark:
					capture_session.record_dark_frame(frame_result.brightness.darkness);
					continue;

				case howdy::native::CompareFrameStatus::kInvalidInput:
					std::cerr << frame_result.error_message << "\n";
					return CompareExit::kInvalidDevice;

				case howdy::native::CompareFrameStatus::kInvalidPreprocessed:
					std::cerr << frame_result.error_message << "\n";
					return CompareExit::kAbort;

				case howdy::native::CompareFrameStatus::kReady:
					capture_session.record_ready_frame(frame_result.brightness.darkness);
					break;
			}

			const auto inference_result =
			    compare_engine.process_face_frame(frame_result.working_frame);

			switch (inference_result.status) {
				case howdy::native::CompareInferenceStatus::kNoMatch:
					break;

				case howdy::native::CompareInferenceStatus::kMatch:
					winning_index = inference_result.winning_index;
					winning_score = inference_result.winning_score;

					if (context.end_report) {
						const auto &stats = capture_session.stats();
						const auto  total_ms =
						    std::chrono::duration_cast<std::chrono::milliseconds>(
						        std::chrono::steady_clock::now() - context.start_time)
						        .count();
						std::cout << "Total time: " << total_ms << "ms\n";
						std::cout << "Frames searched: " << stats.frames << "\n";
						std::cout << "Black frames ignored: " << stats.black_frames << "\n";
						std::cout << "Dark frames ignored: " << stats.dark_frames << "\n";
						std::cout << "Winning score: " << winning_score << "\n";
						if (winning_index >= 0 &&
						    std::cmp_less(winning_index, context.stored_encodings.models.size())) {
							const auto &winner =
							    context.stored_encodings
							        .models[static_cast<std::size_t>(winning_index)];
							std::cout << "Winning model: " << winner.id << " (\"" << winner.label
							          << "\")\n";
						}
					}

					return CompareExit::kSuccess;

				case howdy::native::CompareInferenceStatus::kInvalidPreparedFrame:
				case howdy::native::CompareInferenceStatus::kDetectionFailed:
				case howdy::native::CompareInferenceStatus::kEncodingFailed:
				case howdy::native::CompareInferenceStatus::kInvalidMatchResult:
					std::cerr << inference_result.error_message << "\n";
					return CompareExit::kAbort;

				case howdy::native::CompareInferenceStatus::kInvalidDependencies:
					return CompareExit::kAbort;
			}

			capture_session.restore_exposure();
		}
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

		const auto sandbox_result = howdy::native::apply_compare_sandbox(config.video.timeout);
		if (sandbox_result.status != howdy::native::CompareSandboxStatus::kOk) {
			report_sandbox_failure(sandbox_result);
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

		howdy::native::CompareCaptureSession capture_session(config.video);

		CompareProductionContext processing_context{
		    .capture_session  = capture_session,
		    .face_model       = face_model,
		    .video_config     = config.video,
		    .stored_encodings = loaded_models.stored,
		    .start_time       = start_time,
		    .end_report       = config.debug.end_report,
		};
		const auto processing_result =
		    howdy::native::compare_processing_internal::run_compare_processing({
		        .context          = &processing_context,
		        .open_capture     = open_capture,
		        .drop_privileges  = drop_privileges,
		        .construct_engine = construct_engine,
		        .reset_timeout    = reset_timeout,
		        .run_frame_loop   = run_frame_loop,
		    });
		if (std::holds_alternative<
		        howdy::native::compare_processing_internal::CompareProcessingInvalidDependencies>(
		        processing_result)) {
			std::cerr << "compare processing operations unavailable\n";
			return static_cast<int>(CompareExit::kAbort);
		}
		if (const auto *capture_open =
		        std::get_if<howdy::native::CompareCaptureOpenResult>(&processing_result)) {
			switch (capture_open->status) {
				case howdy::native::CompareCaptureOpenStatus::kOpenFailed:
					std::cerr << capture_open->error_message << "\n";
					return static_cast<int>(CompareExit::kInvalidDevice);
				case howdy::native::CompareCaptureOpenStatus::kInvalidDependencies:
				case howdy::native::CompareCaptureOpenStatus::kOk:
					return static_cast<int>(CompareExit::kAbort);
			}
		}
		if (const auto *privilege_result =
		        std::get_if<howdy::native::ComparePrivilegeResult>(&processing_result)) {
			if (!privilege_result->ok()) {
				std::cerr << "Failed to drop compare privileges: "
				          << privilege_result->error_message << "\n";
			}
			return static_cast<int>(CompareExit::kAbort);
		}
		return static_cast<int>(std::get<CompareExit>(processing_result));

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
