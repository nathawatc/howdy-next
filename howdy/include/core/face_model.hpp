#pragma once

#include "config/runtime_config.hpp"
#include "core/face_detection.hpp"
#include "core/face_encoding.hpp"
#include "core/face_matching.hpp"

#include <memory>
#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/objdetect/face.hpp>

namespace howdy::native {
	enum class FaceModelErrorCategory {
		kNone,
		kModelNotReady,
		kDetectorInitialization,
		kRecognizerInitialization,
	};

	class FaceModel {
	public:
		static constexpr auto kBackendName = "opencv_dnn_sface";
		static constexpr auto kYunetModel  = "face_detection_yunet_2026may.onnx";
		static constexpr auto kSfaceModel  = "face_recognition_sface_2021dec_int8.onnx";

		explicit FaceModel(const FaceConfig &config);

		[[nodiscard]] auto ok() const -> bool;
		[[nodiscard]] auto error_category() const -> FaceModelErrorCategory;
		[[nodiscard]] auto error_message() const -> const std::string &;
		[[nodiscard]] auto metric() const -> const std::string &;

		[[nodiscard]] auto prepare_frame(const cv::Mat &frame) const -> cv::Mat;
		auto               detect(const cv::Mat &frame) -> FaceDetectionResult;
		auto encode(const cv::Mat &frame, const FaceDetection &face) -> FaceEncodingResult;
		[[nodiscard]] auto best_match(const std::vector<std::vector<float>> &known,
		                              const std::vector<float> &probe) const -> FaceMatch;

	private:
		struct Backend;

		friend class FaceModelBackendFactory;

		FaceModel(const FaceConfig &config, Backend backend);
		void initialize(const FaceConfig &config);
		void set_input_size_from_frame(const cv::Mat &frame);
		void set_error(FaceModelErrorCategory category, std::string message);

		bool                          ok_             = false;
		FaceModelErrorCategory        error_category_ = FaceModelErrorCategory::kNone;
		std::string                   error_message_;
		std::string                   metric_    = "cosine";
		float                         threshold_ = 0.363F;
		cv::Size                      input_size_{320, 320};
		cv::Ptr<cv::FaceDetectorYN>   detector_;
		cv::Ptr<cv::FaceRecognizerSF> recognizer_;
		std::shared_ptr<Backend>      backend_;
	};

}  // namespace howdy::native
