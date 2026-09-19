#pragma once

#include "vision/face_detection.hpp"
#include "vision/face_encoding.hpp"

#include <functional>

#include <opencv2/core.hpp>

namespace howdy::native {

	struct FaceAlignmentRequest {
		std::reference_wrapper<const cv::Mat> frame;
		std::reference_wrapper<const cv::Mat> face;
		std::reference_wrapper<cv::Mat>       aligned;
	};

	using AlignFaceFn          = void (*)(void *context, const FaceAlignmentRequest &request);
	using ExtractFaceFeatureFn = void (*)(void *context, const cv::Mat &aligned, cv::Mat &feature);

	struct FaceEncodingDependencies {
		void                *context         = nullptr;
		AlignFaceFn          align_face      = nullptr;
		ExtractFaceFeatureFn extract_feature = nullptr;
	};

	[[nodiscard]] auto EncodeSface(const cv::Mat &frame, const FaceDetection &face,
	                               const FaceEncodingDependencies &dependencies)
	    -> FaceEncodingResult;

}  // namespace howdy::native
