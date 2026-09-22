#pragma once

#include "config/runtime_config.hpp"
#include "model_assets/opencv_model_manifest.hpp"
#include "vision/face_detection.hpp"
#include "vision/face_encoding.hpp"
#include "vision/face_inference.hpp"

#include <memory>
#include <string>

#include <opencv2/core.hpp>
#include <opencv2/objdetect/face.hpp>

namespace howdy::native {
	// Call before starting parallel model and camera initialization.
	[[nodiscard]] auto ConfigureFaceModelEnvironment() -> bool;

	inline constexpr auto kFaceModelNotInitializedMessage = "Face model was not initialized";

	class FaceModel {
	public:
		static constexpr auto        kBackendName = "opencv_dnn_sface";
		static constexpr const char *kYunetModel  = kYunetModelDescriptor.filename.data();
		static constexpr const char *kSfaceModel  = kSfaceModelDescriptor.filename.data();

		explicit FaceModel(const FaceConfig &config);
		~FaceModel();

		FaceModel(const FaceModel &)                     = delete;
		auto operator=(const FaceModel &) -> FaceModel & = delete;

		FaceModel(FaceModel &&) noexcept;
		auto operator=(FaceModel &&) noexcept -> FaceModel &;

		[[nodiscard]] auto Ok() const -> bool;
		[[nodiscard]] auto ErrorMessage() const -> const std::string &;
		[[nodiscard]] auto Metric() const -> FaceMetric;

		[[nodiscard]] static auto PrepareFrame(const cv::Mat &frame) -> cv::Mat;
		auto                      Detect(const cv::Mat &frame) -> FaceDetectionResult;
		auto Encode(const cv::Mat &frame, const FaceDetection &face) -> FaceEncodingResult;

	private:
		struct Backend;

		friend class FaceModelBackendFactory;

		FaceModel(const FaceConfig &config, Backend backend);
		void Initialize(const FaceConfig &config);
		void SetInputSizeFromFrame(const cv::Mat &frame);
		void SetError(std::string message);

		bool                          ok_ = false;
		std::string                   error_message_;
		FaceMetric                    metric_ = FaceMetric::kCosine;
		cv::Size                      input_size_{320, 320};
		cv::Ptr<cv::FaceDetectorYN>   detector_;
		cv::Ptr<cv::FaceRecognizerSF> recognizer_;
		std::unique_ptr<Backend>      backend_;
	};

	[[nodiscard]] auto FaceModelInferenceOperations(FaceModel &model) -> FaceInferenceOperations;

}  // namespace howdy::native
