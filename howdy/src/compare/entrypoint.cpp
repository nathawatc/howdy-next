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
#include "protocol/compare_descriptors.hpp"
#include "protocol/compare_exit.hpp"
#include "protocol/compare_verdict.hpp"
#include "storage/user_model_status.hpp"
#include "storage/user_model_types.hpp"
#include "storage/user_models.hpp"
#include "support/bit_flags.hpp"
#include "support/cpu_topology.hpp"
#include "support/eintr.hpp"
#include "support/error_message.hpp"
#include "support/scoped_fd.hpp"
#include "vision/face_inference.hpp"
#include "vision/face_model.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <fcntl.h>
#include <unistd.h>

#include <opencv2/core/exception.hpp>
#include <opencv2/core/utility.hpp>

#include <sys/socket.h>
#include <sys/stat.h>
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

	void EmitStderrWithErrno(std::string_view message, int error_number) {
		std::cerr << message;
		if (error_number != 0) {
			std::cerr << ": " << howdy::native::SystemErrorMessage(error_number);
		}
		std::cerr << "\n";
	}

	void ReportSandboxFailure(const howdy::native::CompareSandboxResult &result) {
		switch (result.status) {
			case howdy::native::CompareSandboxStatus::kOk:
				return;
			case howdy::native::CompareSandboxStatus::kNoNewPrivilegesFailure:
				EmitStderrWithErrno("Failed to enable no_new_privs sandboxing",
				                    result.error_number);
				return;
			case howdy::native::CompareSandboxStatus::kLimitInspectionFailure:
				EmitStderrWithErrno(std::string("Failed to inspect ") +
				                        SandboxResourceName(result.resource) + " sandbox limit",
				                    result.error_number);
				return;
			case howdy::native::CompareSandboxStatus::kLimitBelowMinimum:
				std::cerr << "Inherited " << SandboxResourceName(result.resource)
				          << " limit is below Howdy's minimum sandbox policy\n";
				return;
			case howdy::native::CompareSandboxStatus::kLimitApplicationFailure:
				EmitStderrWithErrno(std::string("Failed to apply ") +
				                        SandboxResourceName(result.resource) + " sandbox limit",
				                    result.error_number);
				return;
			case howdy::native::CompareSandboxStatus::kInvalidDependencies:
				EmitStderrWithErrno("Invalid compare sandbox dependencies", result.error_number);
				return;
		}
	}

	void ReportSeccompFailure(const howdy::native::CompareSeccompResult &result) {
		switch (result.status) {
			case howdy::native::CompareSeccompStatus::kOk:
				return;
			case howdy::native::CompareSeccompStatus::kUnsupported:
				EmitStderrWithErrno("Seccomp filter sandboxing is not supported by the kernel",
				                    result.error_number);
				return;
			case howdy::native::CompareSeccompStatus::kFilterConstructionFailure:
				EmitStderrWithErrno("Failed to construct compare seccomp filter",
				                    result.error_number);
				return;
			case howdy::native::CompareSeccompStatus::kInstallationFailure:
				EmitStderrWithErrno("Failed to install compare seccomp filter",
				                    result.error_number);
				return;
			case howdy::native::CompareSeccompStatus::kInvalidDependencies:
				EmitStderrWithErrno("Invalid compare seccomp dependencies", result.error_number);
				return;
		}
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
		auto const &model = context.face_model.emplace(context.face_config.get());
		if (!model.Ok()) {
			std::cerr << model.ErrorMessage() << "\n";
			return false;
		}
		return true;
	}

	auto OpenCapture(void *raw_context) -> howdy::native::CompareCaptureOpenResult {
		auto const &context = *static_cast<CompareProductionContext *>(raw_context);
		return context.capture_session.get().Open();
	}

	auto DropPrivileges(void *raw_context) -> howdy::native::ComparePrivilegeResult {
		auto const &context = *static_cast<CompareProductionContext *>(raw_context);
		if (context.dependencies != nullptr) {
			if (context.dependencies->drop_privileges == nullptr) {
				return {
				    .status        = howdy::native::ComparePrivilegeStatus::kVerificationFailure,
				    .error_message = "Injected compare privilege operation unavailable",
				};
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
		auto const &context = *static_cast<CompareProductionContext *>(raw_context);
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

	auto ApplySeccomp(const CompareProductionContext &context)
	    -> howdy::native::CompareSeccompResult {
		if (context.dependencies != nullptr) {
			return context.dependencies->apply_seccomp == nullptr
			           ? howdy::native::CompareSeccompResult{}
					   : context.dependencies->apply_seccomp(context.dependencies->context);
		}
		return howdy::native::ApplyCompareSeccompFilter();
	}

	auto RunFrameLoop(void *raw_context, std::function<void()> &before_first_encode)
	    -> CompareExit {
		auto &context = *static_cast<CompareProductionContext *>(raw_context);
		if (!context.compare_engine.has_value()) {
			return CompareExit::kAbort;
		}
		auto &capture_session = context.capture_session.get();
		auto &compare_engine  = *context.compare_engine;

		const auto seccomp_result = ApplySeccomp(context);
		if (seccomp_result.status != howdy::native::CompareSeccompStatus::kOk) {
			ReportSeccompFailure(seccomp_result);
			// Only a kernel without seccomp is tolerated; a filter that failed to build or
			// install must not leave the scan running unsandboxed.
			if (seccomp_result.status != howdy::native::CompareSeccompStatus::kUnsupported) {
				return CompareExit::kAbort;
			}
		}

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

	// The verdict socket as found at startup, so the report can check that the descriptor still
	// names it and never writes into a file that took the number meanwhile.
	struct VerdictSocket {
		bool  announced = false;
		dev_t device    = 0;
		ino_t inode     = 0;
	};

	// The verdict socket PAM handed compare (protocol/compare_verdict.hpp), if any: the
	// environment announces it and the descriptor is an AF_UNIX SOCK_SEQPACKET socket open for
	// reading and writing. Checked before compare opens anything, so the descriptor cannot be one
	// of compare's own files.
	auto FindVerdictSocket() noexcept -> VerdictSocket {
		using howdy::native::compare_verdict::kFd;
		const char *const descriptor =
		    std::getenv(howdy::native::compare_verdict::kEnvironmentName);
		if (descriptor == nullptr ||
		    std::string_view(descriptor) != howdy::native::compare_verdict::kEnvironmentValue) {
			return {};
		}
		struct stat status{};
		const int   flags = fcntl(kFd, F_GETFL);
		if (flags < 0 ||
		    howdy::native::MaskedBits(flags, O_ACCMODE) !=
		        howdy::native::BitRepresentation(O_RDWR) ||
		    fstat(kFd, &status) != 0 || !S_ISSOCK(status.st_mode)) {
			return {};
		}
		int       domain = -1;
		int       type   = -1;
		socklen_t length = sizeof(domain);
		if (getsockopt(kFd, SOL_SOCKET, SO_DOMAIN, &domain, &length) != 0 || domain != AF_UNIX) {
			return {};
		}
		length = sizeof(type);
		if (getsockopt(kFd, SOL_SOCKET, SO_TYPE, &type, &length) != 0 || type != SOCK_SEQPACKET) {
			return {};
		}
		return {.announced = true, .device = status.st_dev, .inode = status.st_ino};
	}

	// Also reports the exit code on the verdict socket, so PAM still learns it if the application
	// reaps compare first. Best effort: a failed write leaves PAM with only the exit status, which
	// fails closed when it is gone.
	auto ReportVerdict(const VerdictSocket &socket, int exit_code) noexcept -> int {
		struct stat status{};
		if (socket.announced && fstat(howdy::native::compare_verdict::kFd, &status) == 0 &&
		    S_ISSOCK(status.st_mode) && status.st_dev == socket.device &&
		    status.st_ino == socket.inode) {
			const auto verdict = static_cast<unsigned char>(exit_code);
			(void)howdy::native::RetryOnEintr([&] -> ssize_t {
				return write(howdy::native::compare_verdict::kFd, &verdict, sizeof(verdict));
			});
		}
		return exit_code;
	}

	// The config and model descriptors PAM passed (protocol/compare_descriptors.hpp). They are
	// the only source compare reads them from; anything missing or unusable fails closed.
	struct RuntimeDescriptors {
		enum class Mode : std::uint8_t {
			kReady,
			kNotAnnounced,
			kNotOpen,
			kNoVerdict,
		};

		Mode                    mode = Mode::kNotAnnounced;
		howdy::native::ScopedFd config;
		howdy::native::ScopedFd model;
	};

	// Takes the descriptors the environment announces, before compare opens anything, so neither
	// number can belong to one of compare's own files. Each descriptor announced with its exact
	// value is compare's from here on and is closed on every path; type, owner, mode and link
	// checks are left to the loaders.
	auto TakeRuntimeDescriptors(bool verdict_announced) noexcept -> RuntimeDescriptors {
		namespace descriptors = howdy::native::compare_descriptors;
		const auto announced  = [](const char *name, std::string_view expected) -> bool {
			const char *const value = std::getenv(name);
			return value != nullptr && std::string_view(value) == expected;
		};
		const auto take = [](int fd, howdy::native::ScopedFd *owner) -> bool {
			if (fcntl(fd, F_GETFD) < 0) {
				return false;
			}
			owner->Reset(fd);
			return true;
		};
		RuntimeDescriptors result;
		const bool         config_announced =
		    announced(descriptors::kConfigEnvironmentName, descriptors::kConfigEnvironmentValue);
		const bool model_announced =
		    announced(descriptors::kModelEnvironmentName, descriptors::kModelEnvironmentValue);
		const bool config_open = config_announced && take(descriptors::kConfigFd, &result.config);
		const bool model_open  = model_announced && take(descriptors::kModelFd, &result.model);
		if (!config_announced || !model_announced) {
			result.mode = RuntimeDescriptors::Mode::kNotAnnounced;
		} else if (!config_open || !model_open) {
			result.mode = RuntimeDescriptors::Mode::kNotOpen;
		} else if (!verdict_announced) {
			result.mode = RuntimeDescriptors::Mode::kNoVerdict;
		} else {
			result.mode = RuntimeDescriptors::Mode::kReady;
		}
		return result;
	}

	// Why the descriptors cannot be used, or nullopt when compare may go on.
	auto RuntimeDescriptorsError(const RuntimeDescriptors &descriptors)
	    -> std::optional<std::string> {
		namespace protocol = howdy::native::compare_descriptors;
		using Mode         = RuntimeDescriptors::Mode;
		switch (descriptors.mode) {
			case Mode::kReady:
				return std::nullopt;
			case Mode::kNotAnnounced:
				return std::string(
				           "Compare needs its config and model descriptors, announced as ") +
				       protocol::kConfigEnvironmentEntry + " and " +
				       protocol::kModelEnvironmentEntry;
			case Mode::kNotOpen:
				return std::string("An announced compare descriptor is not open");
			case Mode::kNoVerdict:
				return std::string("Compare descriptors require the verdict socket");
		}
		return std::string("Invalid compare descriptors");
	}

	// Loads the runtime config from its descriptor and closes it.
	auto LoadCompareConfig(RuntimeDescriptors &descriptors, const uid_t owner_uid)
	    -> howdy::native::RuntimeConfigLoadResult {
		auto result = howdy::native::LoadRuntimeConfigFromFd(
		    descriptors.config.Get(),
		    std::string("config descriptor ") +
		        howdy::native::compare_descriptors::kConfigEnvironmentValue,
		    owner_uid);
		descriptors.config.Reset();
		return result;
	}

	// Loads the user's models from their descriptor and closes it.
	auto LoadCompareModels(RuntimeDescriptors &descriptors, const uid_t owner_uid)
	    -> howdy::native::UserModelLoadResult {
		auto result = howdy::native::LoadUserModelsFromFd(
		    descriptors.model.Get(),
		    std::string("user model descriptor ") +
		        howdy::native::compare_descriptors::kModelEnvironmentValue,
		    howdy::native::FaceModel::kBackendName, owner_uid);
		descriptors.model.Reset();
		return result;
	}

	auto RunCompareEntrypointBody(int argc, char **argv, const uid_t trusted_owner_uid,
	                              const howdy::native::CompareEntrypointDependencies *dependencies,
	                              RuntimeDescriptors &descriptors) -> int {
		try {
			const auto start_time   = std::chrono::steady_clock::now();
			const auto parse_result = howdy::native::ParseCompareArgs(argc, argv);
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
			if (const auto error = RuntimeDescriptorsError(descriptors)) {
				std::cerr << *error << "\n";
				return static_cast<int>(CompareExit::kAbort);
			}
			auto config_result = LoadCompareConfig(descriptors, trusted_owner_uid);
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

			auto loaded_models = LoadCompareModels(descriptors, trusted_owner_uid);
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

			cv::setNumThreads(howdy::native::AvailableThreadCount());

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

	// Every entry point goes through here, so the verdict socket and the runtime descriptors are
	// checked once, before compare opens anything, and the verdict is reported once, after the
	// exit code is known.
	auto RunCompareEntrypointImpl(int argc, char **argv, const uid_t trusted_owner_uid,
	                              const howdy::native::CompareEntrypointDependencies *dependencies)
	    -> int {
		const VerdictSocket socket      = FindVerdictSocket();
		RuntimeDescriptors  descriptors = TakeRuntimeDescriptors(socket.announced);
		const int           exit_code =
		    RunCompareEntrypointBody(argc, argv, trusted_owner_uid, dependencies, descriptors);
		// A body that returned early leaves them open; close them before the verdict.
		descriptors.config.Reset();
		descriptors.model.Reset();
		// Flushing can still end compare (SIGPIPE on a closed output), so it happens before the
		// verdict, which has to be the last thing compare does.
		std::cout.flush();
		std::cerr.flush();
		(void)std::fflush(nullptr);
		return ReportVerdict(socket, exit_code);
	}

}  // namespace

auto howdy::native::RunCompareEntrypoint(int argc, char **argv, const uid_t trusted_owner_uid)
    -> int {
	return RunCompareEntrypointImpl(argc, argv, trusted_owner_uid, nullptr);
}

auto howdy::native::RunCompareEntrypoint(int argc, char **argv, const uid_t trusted_owner_uid,
                                         const CompareEntrypointDependencies &dependencies) -> int {
	return RunCompareEntrypointImpl(argc, argv, trusted_owner_uid, &dependencies);
}
