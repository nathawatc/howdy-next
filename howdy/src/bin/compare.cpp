#include "compare/args.hpp"
#include "compare/capture_session.hpp"
#include "compare/engine.hpp"
#include "compare/logic.hpp"
#include "compare/processing.hpp"
#include "compare/sandbox.hpp"
#include "config/runtime_config.hpp"
#include "config/runtime_paths.hpp"
#include "protocol/compare_exit.hpp"
#include "storage/user_model_status.hpp"
#include "storage/user_models.hpp"
#include "vision/face_model.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <utility>

#include <opencv2/core.hpp>

namespace {

	using howdy::native::CompareExit;

	auto SandboxResourceName(howdy::native::CompareSandboxResource resource) -> const char * {
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

	void ReportSandboxFailure(const howdy::native::CompareSandboxResult &result) {
		switch (result.status) {
			case howdy::native::CompareSandboxStatus::kOk:
				return;
			case howdy::native::CompareSandboxStatus::kNoNewPrivilegesFailure:
				std::cerr << "Failed to enable no_new_privs sandboxing";
				break;
			case howdy::native::CompareSandboxStatus::kLimitInspectionFailure:
				std::cerr << "Failed to inspect " << SandboxResourceName(result.resource)
				          << " sandbox limit";
				break;
			case howdy::native::CompareSandboxStatus::kLimitBelowMinimum:
				std::cerr << "Inherited " << SandboxResourceName(result.resource)
				          << " limit is below Howdy's minimum sandbox policy\n";
				return;
			case howdy::native::CompareSandboxStatus::kLimitApplicationFailure:
				std::cerr << "Failed to apply " << SandboxResourceName(result.resource)
				          << " sandbox limit";
				break;
		}
		if (result.error_number != 0) {
			std::cerr << ": " << std::strerror(result.error_number);
		}
		std::cerr << "\n";
	}

	auto PrepareFaceFrameDependency(void *context, const cv::Mat &frame) -> cv::Mat {
		(void)context;
		return howdy::native::FaceModel::PrepareFrame(frame);
	}

	auto DetectFacesDependency(void *context, const cv::Mat &frame)
	    -> howdy::native::FaceDetectionResult {
		return static_cast<howdy::native::FaceModel *>(context)->Detect(frame);
	}

	auto EncodeFaceDependency(void *context, const cv::Mat &frame,
	                          const howdy::native::FaceDetection &face)
	    -> howdy::native::FaceEncodingResult {
		return static_cast<howdy::native::FaceModel *>(context)->Encode(frame, face);
	}

	auto FindBestMatchDependency(void *context, const std::vector<std::vector<float>> &known,
	                             const std::vector<float> &probe) -> howdy::native::FaceMatch {
		return static_cast<howdy::native::FaceModel *>(context)->BestMatch(known, probe);
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

	auto OpenCapture(void *raw_context) -> howdy::native::CompareCaptureOpenResult {
		auto &context = *static_cast<CompareProductionContext *>(raw_context);
		return context.capture_session.Open();
	}

	auto DropPrivileges([[maybe_unused]] void *raw_context)
	    -> howdy::native::ComparePrivilegeResult {
		return howdy::native::DropComparePrivileges();
	}

	void ConstructEngine(void *raw_context) {
		auto &context = *static_cast<CompareProductionContext *>(raw_context);
		context.compare_engine.emplace(context.video_config,
		                               howdy::native::CompareInferenceDependencies{
		                                   .context            = &context.face_model,
		                                   .prepare_face_frame = PrepareFaceFrameDependency,
		                                   .detect_faces       = DetectFacesDependency,
		                                   .encode_face        = EncodeFaceDependency,
		                                   .find_best_match    = FindBestMatchDependency,
		                               },
		                               context.stored_encodings.encodings);
	}

	void ResetTimeout(void *raw_context) {
		auto &context = *static_cast<CompareProductionContext *>(raw_context);
		context.capture_session.ResetTimeoutClock();
	}

	auto HandleCaptureResult(CompareProductionContext                       &context,
	                         const howdy::native::CompareCaptureFrameResult &result)
	    -> std::optional<CompareExit> {
		switch (result.status) {
			case howdy::native::CompareCaptureFrameStatus::kFrameReady:
				return std::nullopt;
			case howdy::native::CompareCaptureFrameStatus::kTimeout: {
				const auto &stats = context.capture_session.Stats();
				const auto  exit_code =
				    howdy::native::TimeoutExit(stats.dark_frames, stats.valid_frames);
				if (exit_code == CompareExit::kTooDark) {
					std::cerr << howdy::native::kAllFramesTooDarkMessage << '\n';
					std::cerr << howdy::native::kAverageDarknessLabel
					          << (stats.dark_running_total / std::max(stats.valid_frames, 1))
					          << howdy::native::kThresholdLabel
					          << context.video_config.dark_threshold << '\n';
				}
				return exit_code;
			}
			case howdy::native::CompareCaptureFrameStatus::kReadFailed:
				std::cerr << result.error_message << "\n";
				return CompareExit::kInvalidDevice;
			case howdy::native::CompareCaptureFrameStatus::kNotOpen:
			case howdy::native::CompareCaptureFrameStatus::kInvalidDependencies:
				return CompareExit::kAbort;
		}
		return CompareExit::kAbort;
	}

	enum class FrameHandling : std::uint8_t {
		kInfer,
		kSkip,
		kAbort,
		kInvalidDevice,
	};

	auto HandleFrameResult(howdy::native::CompareCaptureSession    &capture_session,
	                       const howdy::native::CompareFrameResult &result) -> FrameHandling {
		switch (result.status) {
			case howdy::native::CompareFrameStatus::kBlackFrame:
				capture_session.RecordBlackFrame();
				return FrameHandling::kSkip;
			case howdy::native::CompareFrameStatus::kTooDark:
				capture_session.RecordDarkFrame(result.brightness.darkness);
				return FrameHandling::kSkip;
			case howdy::native::CompareFrameStatus::kInvalidInput:
				std::cerr << result.error_message << "\n";
				return FrameHandling::kInvalidDevice;
			case howdy::native::CompareFrameStatus::kInvalidPreprocessed:
				std::cerr << result.error_message << "\n";
				return FrameHandling::kAbort;
			case howdy::native::CompareFrameStatus::kReady:
				capture_session.RecordReadyFrame(result.brightness.darkness);
				return FrameHandling::kInfer;
		}
		return FrameHandling::kAbort;
	}

	void EmitSuccessReport(const CompareProductionContext              &context,
	                       const howdy::native::CompareInferenceResult &result) {
		if (!context.end_report) {
			return;
		}
		const auto &stats    = context.capture_session.Stats();
		const auto  total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
		                           std::chrono::steady_clock::now() - context.start_time)
		                           .count();
		std::cout << "Total time: " << total_ms << "ms\n";
		std::cout << "Frames searched: " << stats.frames << "\n";
		std::cout << "Black frames ignored: " << stats.black_frames << "\n";
		std::cout << "Dark frames ignored: " << stats.dark_frames << "\n";
		std::cout << "Winning score: " << result.winning_score << "\n";
		if (result.winning_index >= 0 &&
		    std::cmp_less(result.winning_index, context.stored_encodings.models.size())) {
			const auto &winner =
			    context.stored_encodings.models[static_cast<std::size_t>(result.winning_index)];
			std::cout << "Winning model: " << winner.id << " (\"" << winner.label << "\")\n";
		}
	}

	auto HandleInferenceResult(const CompareProductionContext              &context,
	                           const howdy::native::CompareInferenceResult &result)
	    -> std::optional<CompareExit> {
		switch (result.status) {
			case howdy::native::CompareInferenceStatus::kNoMatch:
				return std::nullopt;
			case howdy::native::CompareInferenceStatus::kMatch:
				EmitSuccessReport(context, result);
				return CompareExit::kSuccess;
			case howdy::native::CompareInferenceStatus::kInvalidPreparedFrame:
			case howdy::native::CompareInferenceStatus::kDetectionFailed:
			case howdy::native::CompareInferenceStatus::kEncodingFailed:
			case howdy::native::CompareInferenceStatus::kInvalidMatchResult:
				std::cerr << result.error_message << "\n";
				return CompareExit::kAbort;
			case howdy::native::CompareInferenceStatus::kInvalidDependencies:
				return CompareExit::kAbort;
		}
		return CompareExit::kAbort;
	}

	auto RunFrameLoop(void *raw_context) -> CompareExit {
		auto &context = *static_cast<CompareProductionContext *>(raw_context);
		if (!context.compare_engine.has_value()) {
			return CompareExit::kAbort;
		}
		auto &capture_session = context.capture_session;
		auto &compare_engine  = *context.compare_engine;

		while (true) {
			auto capture_result = capture_session.NextFrame();
			if (const auto exit = HandleCaptureResult(context, capture_result); exit.has_value()) {
				return *exit;
			}

			const auto frame_result = compare_engine.ProcessGrayFrame(
			    std::move(capture_result.gray_frame), capture_result.frame_number);

			switch (HandleFrameResult(capture_session, frame_result)) {
				case FrameHandling::kInfer:
					break;
				case FrameHandling::kSkip:
					continue;
				case FrameHandling::kAbort:
					return CompareExit::kAbort;
				case FrameHandling::kInvalidDevice:
					return CompareExit::kInvalidDevice;
			}

			const auto inference_result =
			    compare_engine.ProcessFaceFrame(frame_result.working_frame);

			if (const auto exit = HandleInferenceResult(context, inference_result);
			    exit.has_value()) {
				return *exit;
			}

			capture_session.RestoreExposure();
		}
	}

}  // namespace

auto main(int argc, char **argv) -> int {
	try {
		const auto start_time   = std::chrono::steady_clock::now();
		const auto parse_result = howdy::native::ParseCompareArgs(
		    argc, argv, howdy::native::ResolveConfigPath().string());
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
		    howdy::native::LoadRuntimeConfig(args.config_path, static_cast<uid_t>(0));
		if (config_result.status != howdy::native::RuntimeConfigLoadStatus::kOk ||
		    !config_result.config.has_value()) {
			std::cerr << config_result.error_message << "\n";
			return static_cast<int>(CompareExit::kAbort);
		}
		const auto &config = *config_result.config;

		const auto sandbox_result = howdy::native::ApplyCompareSandbox(config.video.timeout);
		if (sandbox_result.status != howdy::native::CompareSandboxStatus::kOk) {
			ReportSandboxFailure(sandbox_result);
			return static_cast<int>(CompareExit::kAbort);
		}

		const auto loaded_models = howdy::native::LoadUserModels(
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
		if (!face_model.Ok()) {
			std::cerr << face_model.ErrorMessage() << "\n";
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
		    howdy::native::compare_processing_internal::RunCompareProcessing({
		        .context          = &processing_context,
		        .open_capture     = OpenCapture,
		        .drop_privileges  = DropPrivileges,
		        .construct_engine = ConstructEngine,
		        .reset_timeout    = ResetTimeout,
		        .run_frame_loop   = RunFrameLoop,
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
			if (!privilege_result->Ok()) {
				std::cerr << "Failed to drop compare privileges: "
				          << privilege_result->error_message << "\n";
			}
			return static_cast<int>(CompareExit::kAbort);
		}
		return static_cast<int>(std::get<CompareExit>(processing_result));

	} catch (const cv::Exception &error) {
		return static_cast<int>(howdy::native::CompareAbortFromCvException(
		    error, std::cerr, "authentication compare path"));
	} catch (const std::exception &error) {
		return static_cast<int>(howdy::native::CompareAbortFromException(
		    error, std::cerr, "authentication compare path"));
	} catch (...) {
		return static_cast<int>(howdy::native::CompareAbortFromUnknownException(
		    std::cerr, "authentication compare path"));
	}
}
