#pragma once

#include "vision/face_detection.hpp"
#include "vision/face_encoding.hpp"

#include <opencv2/core.hpp>

namespace howdy::native {

	using PrepareFrameFn = cv::Mat (*)(void *context, const cv::Mat &frame);

	using DetectFacesFn = FaceDetectionResult (*)(void *context, const cv::Mat &frame);

	using EncodeFaceFn = FaceEncodingResult (*)(void *context, const cv::Mat &frame,
	                                            const FaceDetection &face);

	struct FaceInferenceOperations {
		void          *context       = nullptr;
		PrepareFrameFn prepare_frame = nullptr;
		DetectFacesFn  detect_faces  = nullptr;
		EncodeFaceFn   encode_face   = nullptr;
	};

	[[nodiscard]] constexpr auto
	FaceInferenceDetectionReady(const FaceInferenceOperations &operations) noexcept -> bool {
		return operations.context != nullptr && operations.prepare_frame != nullptr &&
		       operations.detect_faces != nullptr;
	}

	[[nodiscard]] constexpr auto
	FaceInferenceComplete(const FaceInferenceOperations &operations) noexcept -> bool {
		return FaceInferenceDetectionReady(operations) && operations.encode_face != nullptr;
	}

}  // namespace howdy::native
