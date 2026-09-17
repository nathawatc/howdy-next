#pragma once

#include "vision/face_detection.hpp"
#include "vision/face_encoding.hpp"
#include "vision/face_matching.hpp"

#include <vector>

#include <opencv2/core.hpp>

namespace howdy::native {

	using PrepareFrameFn = cv::Mat (*)(void *context, const cv::Mat &frame);

	using DetectFacesFn = FaceDetectionResult (*)(void *context, const cv::Mat &frame);

	using EncodeFaceFn = FaceEncodingResult (*)(void *context, const cv::Mat &frame,
	                                            const FaceDetection &face);

	using MatchFaceFn = FaceMatch (*)(void *context, const std::vector<std::vector<float>> &known,
	                                  const std::vector<float> &probe);

	struct FaceInferenceOperations {
		void          *context       = nullptr;
		PrepareFrameFn prepare_frame = nullptr;
		DetectFacesFn  detect_faces  = nullptr;
		EncodeFaceFn   encode_face   = nullptr;
		MatchFaceFn    match_face    = nullptr;

		[[nodiscard]] auto DetectionReady() const -> bool {
			return context != nullptr && prepare_frame != nullptr && detect_faces != nullptr;
		}

		[[nodiscard]] auto Complete() const -> bool {
			return DetectionReady() && encode_face != nullptr && match_face != nullptr;
		}
	};

}  // namespace howdy::native
