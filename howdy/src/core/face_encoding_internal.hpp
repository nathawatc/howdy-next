#pragma once

#include "core/face_detection.hpp"
#include "core/face_encoding.hpp"

#include <opencv2/core.hpp>

namespace howdy::native {

	using AlignFaceFn          = void (*)(void *context, const cv::Mat &frame, const cv::Mat &face,
	                                      cv::Mat &aligned);
	using ExtractFaceFeatureFn = void (*)(void *context, const cv::Mat &aligned, cv::Mat &feature);

	struct FaceEncodingDependencies {
		void                *context         = nullptr;
		AlignFaceFn          align_face      = nullptr;
		ExtractFaceFeatureFn extract_feature = nullptr;
	};

	[[nodiscard]] auto encode_sface(const cv::Mat &frame, const FaceDetection &face,
	                                const FaceEncodingDependencies &dependencies)
	    -> FaceEncodingResult;

}  // namespace howdy::native
