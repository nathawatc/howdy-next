#include "compare/entrypoint.hpp"

#include "compare/args.hpp"
#include "compare/capture_session.hpp"
#include "compare/engine.hpp"
#include "compare/entrypoint/dependencies.hpp"
#include "compare/entrypoint/internal.hpp"
#include "compare/logic.hpp"
#include "compare/privileges.hpp"
#include "compare/processing.hpp"
#include "compare/sandbox.hpp"
#include "config/runtime_config.hpp"
#include "config/runtime_config_loader.hpp"
#include "config/runtime_paths.hpp"
#include "protocol/compare_exit.hpp"
#include "storage/user_model_status.hpp"
#include "storage/user_model_types.hpp"
#include "storage/user_models.hpp"
#include "support/cpu_topology.hpp"
#include "support/error_message.hpp"
#include "support/file_security/validation_root.hpp"
#include "vision/face_inference.hpp"
#include "vision/face_model.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <variant>

#include <opencv2/core/exception.hpp>
#include <opencv2/core/utility.hpp>

#include <sys/types.h>

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
			std::cerr << ": " << howdy::native::SystemErrorMessage(result.error_number);
		}
		std::cerr << "\n";
	}

	struct CompareProductionContext {
		std::reference_wrapper<howdy::native::CompareCaptureSession> capture_session;
		std::optional<howdy::native::FaceModel>                      face_model;
		std::reference_wrapper<const howdy::native::FaceConfig>      face_config;
		std::reference_wrapper<const howdy::native::VideoConfig>     video_config;
		std::reference_wrapper<howdy::native::StoredEncodings>       stored_encodings;
		std::chrono::steady_clock::time_point                        start_time;
		bool                                                         end_report   = false;
		const howdy::native::CompareEntrypointDependencies          *dependencies = nullptr;
		std::optional<howdy::native::CompareEngine>                  compare_engine;
	};

	auto InitializeModel(void *raw_context) -> bool {
		auto &context = *static_cast<CompareProductionContext *>(raw_context);
		if (context.dependencies != nullptr) {
			return howdy::native::FaceInferenceComplete(context.dependencies->inference);
		}
		auto &model = context.face_model.emplace(context.face_config.get());
		if (!model.Ok()) {
			std::cerr << model.ErrorMessage() << "\n";
			return false;
		}
		return true;
	}

	auto OpenCapture(void *raw_context) -> howdy::native::CompareCaptureOpenResult {
		auto &context = *static_cast<CompareProductionContext *>(raw_context);
		return context.capture_session.get().Open();
	}

	auto DropPrivileges(void *raw_context) -> howdy::native::ComparePrivilegeResult {
		auto &context = *static_cast<CompareProductionContext *>(raw_context);
		if (context.dependencies != nullptr) {
			if (context.dependencies->drop_privileges == nullptr) {
				return {.status = howdy::native::ComparePrivilegeStatus::kVerificationFailure,
				        .error_message = "Injected compare privilege operation unavailable"};
			}
			return context.dependencies->drop_privileges(context.dependencies->context);
		}
		return howdy::native::DropComparePrivileges();
	}

	void ConstructEngine(void *raw_context) {
		auto &context = *static_cast<CompareProductionContext *>(raw_context);
		howdy::native::FaceInferenceOperations inference;
		if (context.dependencies != nullptr) {
			inference = context.dependencies->inference;
		} else if (context.face_model.has_value()) {
			inference = howdy::native::FaceModelInferenceOperations(*context.face_model);
		} else {
			return;
		}
		context.compare_engine.emplace(
		    context.video_config.get(), inference,
		    howdy::native::PreparedFaceMatcher(std::move(context.stored_encodings.get().encodings),
		                                       context.face_config.get().sface_metric,
		                                       context.face_config.get().sface_threshold));
	}

	void ResetTimeout(void *raw_context) {
		auto &context = *static_cast<CompareProductionContext *>(raw_context);
		context.capture_session.get().ResetTimeoutClock();
	}

	auto WarmModel(void *raw_context) -> bool {
		auto &context = *static_cast<CompareProductionContext *>(raw_context);
		if (context.dependencies != nullptr) {
			return context.dependencies->warm_model == nullptr
			           ? true
			           : context.dependencies->warm_model(context.dependencies->context);
		}
		if (!context.face_model.has_value()) {
			return false;
		}
		return howdy::native::compare_entrypoint_internal::WarmModel(*context.face_model);
	}

	auto HandleCaptureResult(CompareProductionContext                       &context,
	                         const howdy::native::CompareCaptureFrameResult &result)
	    -> std::optional<CompareExit> {
		switch (result.status) {
			case howdy::native::CompareCaptureFrameStatus::kFrameReady:
				return std::nullopt;
			case howdy::native::CompareCaptureFrameStatus::kTimeout: {
				const auto &stats = context.capture_session.get().Stats();
				const auto  exit_code =
				    howdy::native::TimeoutExit(stats.dark_frames, stats.valid_frames);
				if (exit_code == CompareExit::kTooDark) {
					std::cerr << howdy::native::kAllFramesTooDarkMessage << '\n';
					std::cerr << howdy::native::kAverageDarknessLabel
					          << (stats.dark_running_total / std::max(stats.valid_frames, 1))
					          << howdy::native::kThresholdLabel
					          << context.video_config.get().dark_threshold << '\n';
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
		const auto &stats    = context.capture_session.get().Stats();
		const auto  total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
		                           std::chrono::steady_clock::now() - context.start_time)
		                           .count();
		std::cout << "Total time: " << total_ms << "ms\n";
		std::cout << "Frames searched: " << stats.frames << "\n";
		std::cout << "Black frames ignored: " << stats.black_frames << "\n";
		std::cout << "Dark frames ignored: " << stats.dark_frames << "\n";
		std::cout << "Winning score: " << result.winning_score << "\n";
		if (result.winning_index >= 0 &&
		    std::cmp_less(result.winning_index, context.stored_encodings.get().models.size())) {
			const auto &winner = context.stored_encodings.get().models.at(
			    static_cast<std::size_t>(result.winning_index));
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

	auto RunFrameLoop(void *raw_context, std::function<void()> &before_first_encode)
	    -> CompareExit {
		auto &context = *static_cast<CompareProductionContext *>(raw_context);
		if (!context.compare_engine.has_value()) {
			return CompareExit::kAbort;
		}
		auto &capture_session = context.capture_session.get();
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
			    compare_engine.ProcessFaceFrame(frame_result.working_frame, &before_first_encode);

			if (const auto exit = HandleInferenceResult(context, inference_result);
			    exit.has_value()) {
				return *exit;
			}

			capture_session.RestoreExposure();
		}
	}

}  // namespace

auto howdy::native::compare_entrypoint_internal::WarmModel(FaceModel &model) -> bool {
	return model.WarmUpRecognizer();
}

namespace {

	auto RunCompareEntrypointImpl(
	    int argc, char **argv, const uid_t trusted_owner_uid,
	    const howdy::native::file_security_internal::ValidationRoot &validation_root,
	    const howdy::native::CompareEntrypointDependencies          *dependencies) -> int {
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

			auto config_result = howdy::native::LoadRuntimeConfig(
			    args.config_path, trusted_owner_uid, validation_root);
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

			auto loaded_models =
			    howdy::native::LoadUserModels(args.user, howdy::native::FaceModel::kBackendName,
			                                  trusted_owner_uid, validation_root);
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

			if (!howdy::native::ConfigureFaceModelEnvironment()) {
				std::cerr << "OpenCV New DNN graph engine could not be configured\n";
				return static_cast<int>(CompareExit::kAbort);
			}

			if (const auto physical_cores = howdy::native::AvailablePhysicalCoreCount();
			    physical_cores.has_value()) {
				cv::setNumThreads(*physical_cores);
			}

			std::optional<howdy::native::CompareCaptureSession> capture_session;
			if (dependencies == nullptr) {
				capture_session.emplace(config.video);
			} else {
				capture_session.emplace(config.video, dependencies->capture);
			}

			CompareProductionContext processing_context{
			    .capture_session  = std::ref(*capture_session),
			    .face_model       = std::nullopt,
			    .face_config      = std::cref(config.face),
			    .video_config     = std::cref(config.video),
			    .stored_encodings = std::ref(loaded_models.stored),
			    .start_time       = start_time,
			    .end_report       = config.debug.end_report,
			    .dependencies     = dependencies,
			};
			const auto processing_result =
			    howdy::native::compare_processing_internal::RunCompareProcessing({
			        .context          = &processing_context,
			        .initialize_model = InitializeModel,
			        .open_capture     = OpenCapture,
			        .drop_privileges  = DropPrivileges,
			        .construct_engine = ConstructEngine,
			        .reset_timeout    = ResetTimeout,
			        .warm_model       = WarmModel,
			        .run_frame_loop   = RunFrameLoop,
			    });
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
				if (!howdy::native::ComparePrivilegeSucceeded(*privilege_result)) {
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

}  // namespace

auto howdy::native::RunCompareEntrypoint(
    int argc, char **argv, const uid_t trusted_owner_uid,
    const file_security_internal::ValidationRoot &validation_root) -> int {
	return RunCompareEntrypointImpl(argc, argv, trusted_owner_uid, validation_root, nullptr);
}

auto howdy::native::RunCompareEntrypoint(
    int argc, char **argv, const uid_t trusted_owner_uid,
    const file_security_internal::ValidationRoot &validation_root,
    const CompareEntrypointDependencies          &dependencies) -> int {
	return RunCompareEntrypointImpl(argc, argv, trusted_owner_uid, validation_root, &dependencies);
}
