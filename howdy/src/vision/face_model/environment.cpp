#include "vision/face_model/internal.hpp"

#include <array>
#include <string>
#include <string_view>

#include <stdlib.h>

#include <opencv2/core/utils/logger.defines.hpp>
#include <opencv2/core/utils/logger.hpp>
#include <opencv2/core/version.hpp>
#include <opencv2/dnn/dnn.hpp>

namespace howdy::native {
	namespace {

		// Per-version mapping is documented at ForcedDnnEngine() in vision/face_model/internal.hpp.
#if CV_VERSION_MAJOR == 5 && CV_VERSION_MINOR == 0
		constexpr int kForcedEngine = cv::dnn::ENGINE_NEW;
#else
		constexpr int kForcedEngine = cv::dnn::ENGINE_OPENCV;
#endif
		static_assert(kForcedEngine > 0 && kForcedEngine <= 9,
		              "OPENCV_FORCE_DNN_ENGINE value must be a single digit");
		constexpr std::array<char, 2> kForcedEngineValue{
		    static_cast<char>('0' + kForcedEngine),
		    '\0',
		};

		constexpr std::string_view kTargetWarningFile     = "net_impl_backend.cpp";
		constexpr std::string_view kTargetWarningFunction = "setPreferableTarget";
		constexpr std::string_view kTargetWarningMessage =
		    "Targets are not supported by the new graph engine for now";

		// Mirrors OpenCV's stripSourceFilePathPrefix(): keep the full path when it ends in a
		// separator.
		auto SourceFileName(std::string_view file) -> std::string_view {
			const auto separator = file.find_last_of("/\\");
			if (separator == std::string_view::npos || separator + 1 == file.size()) {
				return file;
			}
			return file.substr(separator + 1);
		}

		// Signature fixed by cv::utils::logging::WriteLogMessageExFuncType.
		// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
		void WriteFilteredOpenCvLogMessage(cv::utils::logging::LogLevel level, const char *tag,
		                                   const char *file, int line, const char *func,
		                                   const char *message) {
			if (IsFilteredOpenCvLogMessage(
			        {.level = level, .file = file, .func = func, .message = message})) {
				return;
			}
			// Same layout as OpenCV's default writeLogMessageEx(); writeLogMessage() then adds
			// the level/thread prefix and selects the output stream.
			std::string text;
			if (tag != nullptr) {
				text.append(tag).push_back(' ');
			}
			if (file != nullptr) {
				text.append(SourceFileName(file));
				if (line > 0) {
					text.append(":").append(std::to_string(line));
				}
				text.push_back(' ');
			}
			if (func != nullptr) {
				text.append(func).push_back(' ');
			}
			if (message != nullptr) {
				text.append(message);
			}
			cv::utils::logging::internal::writeLogMessage(level, text.c_str());
		}

	}  // namespace

	auto ForcedDnnEngine() -> const char * {
		return kForcedEngineValue.data();
	}

	auto IsFilteredOpenCvLogMessage(const OpenCvLogRecord &record) -> bool {
		if (!kFiltersOpenCvTargetWarning || record.level != cv::utils::logging::LOG_LEVEL_WARNING ||
		    record.file == nullptr || record.func == nullptr || record.message == nullptr) {
			return false;
		}
		return SourceFileName(record.file) == kTargetWarningFile &&
		       record.func == kTargetWarningFunction && record.message == kTargetWarningMessage;
	}

	void InstallOpenCvLogFilter() {
		if constexpr (kFiltersOpenCvTargetWarning) {
			// Atomic in OpenCV and idempotent, so repeated calls from parallel init are safe.
			cv::utils::logging::internal::replaceWriteLogMessageEx(&WriteFilteredOpenCvLogMessage);
		}
	}

	auto ConfigureFaceModelEnvironment() -> bool {
		InstallOpenCvLogFilter();
		// Avoid environment mutation when compare has configured it before its worker starts.
		const auto *engine = getenv("OPENCV_FORCE_DNN_ENGINE");
		if (engine != nullptr && std::string_view(engine) == ForcedDnnEngine()) {
			return true;
		}
		// Pin the intended engine: OpenCV 5.0 would otherwise fall back to the Classic engine,
		// and an inherited value could select another engine such as ONNX Runtime.
		return setenv("OPENCV_FORCE_DNN_ENGINE", ForcedDnnEngine(), 1) == 0;
	}

}  // namespace howdy::native
