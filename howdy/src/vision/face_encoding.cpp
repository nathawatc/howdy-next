#include "vision/face_encoding_internal.hpp"

#include <cmath>
#include <cstddef>
#include <utility>

namespace howdy::native {

	auto encode_sface(const cv::Mat &frame, const FaceDetection &face,
	                  const FaceEncodingDependencies &dependencies) -> FaceEncodingResult {
		if (dependencies.context == nullptr || dependencies.align_face == nullptr ||
		    dependencies.extract_feature == nullptr) {
			return {
			    .status        = FaceEncodingStatus::kInferenceError,
			    .error_message = kMissingSfaceEncodingDependencyMessage,
			};
		}

		cv::Mat row(1, 15, CV_32FC1);
		row.at<float>(0, 0) = face.box.x;
		row.at<float>(0, 1) = face.box.y;
		row.at<float>(0, 2) = face.box.width;
		row.at<float>(0, 3) = face.box.height;
		for (std::size_t index = 0; index < face.landmarks.size(); ++index) {
			const int column             = 4 + static_cast<int>(index * 2);
			row.at<float>(0, column)     = face.landmarks[index].x;
			row.at<float>(0, column + 1) = face.landmarks[index].y;
		}
		row.at<float>(0, 14) = face.confidence;

		cv::Mat aligned;
		try {
			dependencies.align_face(dependencies.context,
			                        {.frame = frame, .face = row, .aligned = aligned});
		} catch (const cv::Exception &) {
			return {
			    .status        = FaceEncodingStatus::kInferenceError,
			    .error_message = "Face encoding failed during SFace alignment",
			};
		}
		if (aligned.empty()) {
			return {
			    .error_message = "SFace alignment returned an empty image",
			};
		}

		cv::Mat feature;
		try {
			dependencies.extract_feature(dependencies.context, aligned, feature);
		} catch (const cv::Exception &) {
			return {
			    .status        = FaceEncodingStatus::kInferenceError,
			    .error_message = "Face encoding failed during SFace feature extraction",
			};
		}
		if (feature.empty()) {
			return {
			    .error_message = "SFace feature extraction returned an empty encoding",
			};
		}
		if (feature.type() != CV_32FC1 || feature.total() != kSfaceEmbeddingSize) {
			return {
			    .error_message = kInvalidFaceEncodingMessage,
			};
		}

		std::vector<float> encoding;
		encoding.reserve(kSfaceEmbeddingSize);
		for (const float value : cv::Mat_<float>(feature)) {
			if (!std::isfinite(value)) {
				return {
				    .error_message = kInvalidFaceEncodingMessage,
				};
			}
			encoding.push_back(value);
		}
		return {
		    .status        = FaceEncodingStatus::kOk,
		    .encoding      = std::move(encoding),
		    .error_message = {},
		};
	}

}  // namespace howdy::native
