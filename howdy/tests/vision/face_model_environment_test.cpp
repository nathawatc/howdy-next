#include "config/runtime_config.hpp"
#include "model_assets/model_file.hpp"
#include "test_support.hpp"
#include "vision/face_model.hpp"
#include "vision/face_model/internal.hpp"
#include "vision/face_model_test_access.hpp"
#include "vision/face_model_test_support.hpp"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <ranges>
#include <sstream>
#include <string>
#include <utility>

#include <stdlib.h>

#include <opencv2/core/cvstd_wrapper.hpp>
#include <opencv2/core/types.hpp>
#include <opencv2/core/utils/logger.defines.hpp>
#include <opencv2/core/utils/logger.hpp>
#include <opencv2/core/version.hpp>
#include <opencv2/dnn/dnn.hpp>

namespace howdy::test::face_model {
	namespace {

		using howdy::test::Expect;

		constexpr auto kTargetWarningPath = "/build/opencv/modules/dnn/src/net_impl_backend.cpp";
		constexpr auto kTargetWarningFunction = "setPreferableTarget";
		constexpr auto kTargetWarningMessage =
		    "Targets are not supported by the new graph engine for now";

		auto TestOpenCvLogFilterPredicate() -> bool {
			using howdy::native::IsFilteredOpenCvLogMessage;
			using howdy::native::OpenCvLogRecord;
			namespace logging        = cv::utils::logging;
			bool                  ok = true;
			const OpenCvLogRecord target{
			    .level   = logging::LOG_LEVEL_WARNING,
			    .file    = kTargetWarningPath,
			    .func    = kTargetWarningFunction,
			    .message = kTargetWarningMessage,
			};
			ok &= Expect(IsFilteredOpenCvLogMessage(target) ==
			                 howdy::native::kFiltersOpenCvTargetWarning,
			             "target warning is filtered only on affected OpenCV versions");

			auto other_level   = target;
			other_level.level  = logging::LOG_LEVEL_ERROR;
			auto other_file    = target;
			other_file.file    = "/build/opencv/modules/dnn/src/net_impl.cpp";
			auto other_func    = target;
			other_func.func    = "setPreferableBackend";
			auto other_text    = target;
			other_text.message = "Targets are not supported by the new graph engine";
			auto null_file     = target;
			null_file.file     = nullptr;
			auto null_func     = target;
			null_func.func     = nullptr;
			auto null_text     = target;
			null_text.message  = nullptr;
			for (const auto &record : {
			         other_level,
			         other_file,
			         other_func,
			         other_text,
			         null_file,
			         null_func,
			         null_text,
			     }) {
				ok &= Expect(!IsFilteredOpenCvLogMessage(record),
				             "log filter keeps records that differ from the target warning");
			}
			return ok;
		}

		// Removes the "<thread>@<time>" part of OpenCV's "[ WARN:<thread>@<time>] " prefix, which
		// differs between calls.
		auto NormalizeOpenCvLogLine(const std::string &line) -> std::string {
			const auto colon   = line.find(':');
			const auto bracket = line.find("] ");
			if (colon == std::string::npos || bracket == std::string::npos || bracket < colon) {
				return line;
			}
			return line.substr(0, colon) + line.substr(bracket);
		}

		auto CaptureOpenCvLog(cv::utils::logging::LogLevel level, const char *tag, const char *file,
		                      const char *func, const char *message) -> std::string {
			std::ostringstream const          captured;
			const howdy::test::StreamRedirect capture(std::cerr, captured.rdbuf());
			cv::utils::logging::internal::writeLogMessageEx(level, tag, file, 42, func, message);
			return NormalizeOpenCvLogLine(captured.str());
		}

		auto TestOpenCvLogFilterOutput() -> bool {
			namespace logging         = cv::utils::logging;
			bool       ok             = true;
			const auto capture_target = [] -> std::string {
				return CaptureOpenCvLog(logging::LOG_LEVEL_WARNING, "global", kTargetWarningPath,
				                        kTargetWarningFunction, kTargetWarningMessage);
			};
			const auto capture_others = [] -> std::array<std::string, 4> {
				return {
				    CaptureOpenCvLog(logging::LOG_LEVEL_WARNING, "global", kTargetWarningPath,
					                 kTargetWarningFunction,
					                 "Targets are not supported for OpenCL"),
				    CaptureOpenCvLog(logging::LOG_LEVEL_ERROR, "global",
					                 "/build/opencv/modules/dnn/src/onnx/onnx_importer.cpp",
					                 "readNetFromONNX", "DNN/ONNX: failed to load model"),
				    CaptureOpenCvLog(logging::LOG_LEVEL_WARNING, nullptr, nullptr, nullptr,
					                 "untagged warning"),
				    CaptureOpenCvLog(logging::LOG_LEVEL_WARNING, "global", "/", "", "edge path"),
				};
			};

			logging::internal::replaceWriteLogMessageEx(nullptr);
			const auto unfiltered_target = capture_target();
			const auto unfiltered_others = capture_others();
			howdy::native::InstallOpenCvLogFilter();
			const auto filtered_target = capture_target();
			const auto filtered_others = capture_others();
			logging::internal::replaceWriteLogMessageEx(nullptr);

			ok &= Expect(
			    unfiltered_target.starts_with("[ WARN] global net_impl_backend.cpp:42 "
				                              "setPreferableTarget Targets are not supported"),
			    "default OpenCV writer prints the target warning");
			if constexpr (howdy::native::kFiltersOpenCvTargetWarning) {
				ok &= Expect(filtered_target.empty(), "log filter drops the target warning");
			} else {
				ok &= Expect(filtered_target == unfiltered_target,
				             "log filter is not installed on unaffected OpenCV versions");
			}
			for (const auto &[before, after] :
			     std::views::zip(unfiltered_others, filtered_others)) {
				ok &= Expect(!before.empty() && after == before,
				             "log filter forwards other records with OpenCV formatting");
			}
			return ok;
		}

		auto TestDnnEngineOverride(const std::string &forced_engine,
		                           const std::string &conflicting_engine) -> bool {
			bool ok = true;
			for (const bool preconfigure : {false, true}) {
				const ScopedEnvironment environment(kDnnEngineEnv);
				setenv(kDnnEngineEnv, conflicting_engine.c_str(), 1);
				if (preconfigure) {
					ok &= Expect(howdy::native::ConfigureFaceModelEnvironment(),
					             "DNN engine can be configured before parallel initialization");
				}

				std::string readiness_engine;
				std::string detector_engine;
				auto        backend = SuccessfulBackend();
				backend.check_readiness =
				    [&readiness_engine](
				        const std::filesystem::path &) -> howdy::native::OpenCvModelReadiness {
					if (const auto *value = std::getenv(kDnnEngineEnv); value != nullptr) {
						readiness_engine = value;
					}
					return howdy::native::OpenCvModelReadiness{
					    .status = howdy::native::OpenCvModelStatus::kOk,
					};
				};
				backend.create_detector = [&detector_engine](const std::string &, const cv::Size &,
				                                             float, float,
				                                             int) -> cv::Ptr<FakeDetector> {
					if (const auto *value = std::getenv(kDnnEngineEnv); value != nullptr) {
						detector_engine = value;
					}
					return cv::makePtr<FakeDetector>();
				};
				auto const model = howdy::native::FaceModelTestAccess::Create(
				    howdy::native::DefaultFaceConfig(), std::move(backend));
				ok &= Expect(model.Ok(), "engine override model initializes");
				const auto *engine = std::getenv(kDnnEngineEnv);
				ok &= Expect(engine != nullptr && engine == forced_engine,
				             "New DNN graph engine overrides conflicting environment");
				ok &=
				    Expect(readiness_engine == (preconfigure ? forced_engine : conflicting_engine),
					       "readiness checks preserve the preconfigured DNN engine");
				ok &= Expect(detector_engine == forced_engine,
				             "New DNN graph engine is forced before factory call");
			}
			return ok;
		}

		auto TestDnnEngineEnvironment() -> bool {
			bool              ok = true;
			const std::string forced_engine(howdy::native::ForcedDnnEngine());
			// Pin the expected value independently of production so a renumbered enum or a wrong
			// version branch fails here. Every other engine an inherited value could select must be
			// overridden.
#if CV_VERSION_MAJOR == 5 && CV_VERSION_MINOR == 0
			constexpr auto   expected_engine = "2";  // ENGINE_NEW
			const std::array conflicting_engines{
			    cv::dnn::ENGINE_CLASSIC,
			    cv::dnn::ENGINE_AUTO,
			    cv::dnn::ENGINE_ORT,
			};
#else
			constexpr auto   expected_engine = "1";  // ENGINE_OPENCV
			const std::array conflicting_engines{
			    cv::dnn::ENGINE_AUTO,
			    cv::dnn::ENGINE_ORT,
			};
#endif
			ok &= Expect(forced_engine == expected_engine,
			             "forced DNN engine selects the New graph engine for this OpenCV version");
			for (const auto conflicting : conflicting_engines) {
				const std::string conflicting_engine = std::to_string(conflicting);
				ok &= Expect(conflicting_engine != forced_engine,
				             "conflicting DNN engine differs from the forced engine");
				ok &= TestDnnEngineOverride(forced_engine, conflicting_engine);
			}
			return ok;
		}

	}  // namespace

	auto RunFaceModelEnvironmentTests() -> bool {
		bool ok = true;
		ok &= TestDnnEngineEnvironment();
		ok &= TestOpenCvLogFilterPredicate();
		ok &= TestOpenCvLogFilterOutput();
		return ok;
	}

}  // namespace howdy::test::face_model
