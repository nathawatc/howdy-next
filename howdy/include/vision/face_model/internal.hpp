#pragma once

#include "model_assets/model_file.hpp"
#include "vision/face_model.hpp"

#include <filesystem>
#include <functional>
#include <string>

namespace howdy::native {

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
