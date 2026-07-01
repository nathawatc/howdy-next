#pragma once

#include "config/runtime_config.hpp"
#include "core/face_detection.hpp"
#include "core/face_matching.hpp"

#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/objdetect/face.hpp>

namespace howdy::native {

	class FaceModel {
	public:
		static constexpr auto kBackendName = "opencv_dnn_sface";
		static constexpr auto kYunetModel  = "face_detection_yunet_2023mar_int8bq.onnx";
		static constexpr auto kSfaceModel  = "face_recognition_sface_2021dec_int8bq.onnx";

		explicit FaceModel(const FaceConfig &config);

		[[nodiscard]] auto ok() const -> bool;
		[[nodiscard]] auto error_message() const -> const std::string &;
		[[nodiscard]] auto metric() const -> const std::string &;

		[[nodiscard]] auto prepare_frame(const cv::Mat &frame) const -> cv::Mat;
		auto               detect(const cv::Mat &frame) -> FaceDetectionResult;
		auto encode(const cv::Mat &frame, const FaceDetection &face) -> std::vector<float>;
		[[nodiscard]] auto best_match(const std::vector<std::vector<float>> &known,
		                              const std::vector<float> &probe) const -> FaceMatch;

	private:
		void               set_input_size_from_frame(const cv::Mat &frame);
		void               set_error(std::string message);
		[[nodiscard]] auto resolve_model_path(const std::string &value,
		                                      const std::string &fallback) const -> std::string;

		bool                          ok_ = false;
		std::string                   error_message_;
		std::string                   metric_    = "cosine";
		float                         threshold_ = 0.363F;
		cv::Size                      input_size_{320, 320};
		cv::Ptr<cv::FaceDetectorYN>   detector_;
		cv::Ptr<cv::FaceRecognizerSF> recognizer_;
	};

}  // namespace howdy::native
