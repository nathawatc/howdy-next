#pragma once

#include "model_assets/model_file.hpp"
#include "vision/face_model.hpp"

#include <filesystem>
#include <functional>
#include <string>

#include <opencv2/core/utils/logger.defines.hpp>
#include <opencv2/core/version.hpp>

namespace howdy::native {

	// OpenCV 5.0.0 through 5.1.0 log a false-positive warning when YuNet and SFace request the
	// default CPU target on the New graph engine. opencv/opencv#29457 silenced it, but
	// opencv/opencv#29286 reintroduced it for builds without CUDA before 5.1.0 was tagged.
#if CV_VERSION_MAJOR == 5 &&                                                                       \
    (CV_VERSION_MINOR == 0 || (CV_VERSION_MINOR == 1 && CV_VERSION_REVISION == 0))
	inline constexpr bool kFiltersOpenCvTargetWarning = true;
#else
	inline constexpr bool kFiltersOpenCvTargetWarning = false;
#endif

	// One OpenCV log record as passed to writeLogMessageEx(); any pointer may be null.
	struct OpenCvLogRecord {
		cv::utils::logging::LogLevel level   = cv::utils::logging::LOG_LEVEL_SILENT;
		const char                  *file    = nullptr;
		const char                  *func    = nullptr;
		const char                  *message = nullptr;
	};

	// True only for that exact warning: level, source file, function, and message must all match,
	// and only on the affected OpenCV versions.
	[[nodiscard]] auto IsFilteredOpenCvLogMessage(const OpenCvLogRecord &record) -> bool;

	// Routes OpenCV log output through a filter that drops IsFilteredOpenCvLogMessage() records
	// and forwards every other record with OpenCV's own formatting. No-op on unaffected versions.
	void InstallOpenCvLogFilter();

	// OPENCV_FORCE_DNN_ENGINE value that selects the New DNN graph engine for the OpenCV
	// version Howdy was built against: ENGINE_NEW on 5.0, ENGINE_OPENCV on 5.1+. OpenCV 5.1
	// removed the Classic engine and renamed ENGINE_NEW to ENGINE_OPENCV; its value 2 now means
	// ENGINE_ORT (ONNX Runtime), so do not hard-code it.
	[[nodiscard]] auto ForcedDnnEngine() -> const char *;

	struct FaceModel::Backend {
		std::function<OpenCvModelReadiness(const std::filesystem::path &)> check_readiness;
		std::function<cv::Ptr<cv::FaceDetectorYN>(const std::string &, const cv::Size &, float,
		                                          float, int)>
		                                                                  create_detector;
		std::function<cv::Ptr<cv::FaceRecognizerSF>(const std::string &)> create_recognizer;
		std::function<void(const cv::Size &)>                             set_input_size;
		std::function<void(const cv::Mat &, cv::Mat &)>                   detect;
	};

}  // namespace howdy::native
