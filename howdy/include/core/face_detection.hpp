#pragma once

#include <array>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

namespace howdy::native {

	struct FaceDetection {
		cv::Rect2f                 box;
		std::array<cv::Point2f, 5> landmarks;
		float                      confidence = 0.0F;
	};

	enum class FaceDetectionStatus {
		kOk,
		kInferenceError,
		kInvalidOutput,
	};

	struct FaceDetectionResult {
		FaceDetectionStatus        status = FaceDetectionStatus::kOk;
		std::vector<FaceDetection> detections;
		std::string                error_message;

		[[nodiscard]] auto ok() const -> bool {
			return status == FaceDetectionStatus::kOk;
		}
	};

	[[nodiscard]] auto parse_yunet_detections(const cv::Mat &rows) -> FaceDetectionResult;

}  // namespace howdy::native
