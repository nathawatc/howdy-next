#pragma once

#include "model_assets/model_file.hpp"
#include "vision/face_model.hpp"

#include <filesystem>
#include <functional>
#include <string>

namespace howdy::native {

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
