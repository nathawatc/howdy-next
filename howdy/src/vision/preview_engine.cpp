#include "vision/preview_engine.hpp"

#include "config/runtime_config.hpp"
#include "vision/frame_validation.hpp"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <utility>

#include <opencv2/core.hpp>

namespace howdy::native {

	PreviewEngine::PreviewEngine(VideoConfig config, PreviewInferenceDependencies dependencies,
	                             std::vector<std::vector<float>> known_encodings,
	                             std::size_t known_model_count, bool matching_enabled)
	    : config_(std::move(config))
	    , clahe_(make_clahe(config_))
	    , dependencies_(dependencies)
	    , known_encodings_(std::move(known_encodings))
	    , known_model_count_(known_model_count)
	    , matching_enabled_(matching_enabled) {}

	auto PreviewEngine::dependencies_valid() const -> bool {
		return dependencies_.context != nullptr && dependencies_.prepare_frame != nullptr &&
		       dependencies_.detect_faces != nullptr && dependencies_.now != nullptr &&
		       (!matching_enabled_ ||
		        (dependencies_.encode_face != nullptr && dependencies_.match_face != nullptr));
	}

	auto PreviewEngine::encode_faces(const cv::Mat                    &prepared,
	                                 const std::vector<FaceDetection> &detections,
	                                 std::vector<PreviewFaceResult>   &faces,
	                                 std::string &first_error) const -> std::vector<EncodedFace> {
		std::vector<EncodedFace> encodings;
		encodings.reserve(detections.size());
		for (std::size_t index = 0; index < detections.size(); ++index) {
			auto encoding =
			    dependencies_.encode_face(dependencies_.context, prepared, detections[index]);
			if (!encoding.ok()) {
				faces[index].status = PreviewFaceStatus::kEncodingFailed;
				if (first_error.empty()) {
					first_error = encoding.error_message.empty()
					                  ? kInvalidFaceEncodingMessage
					                  : std::move(encoding.error_message);
				}
				continue;
			}
			encodings.push_back({.face_index = index, .encoding = std::move(encoding.encoding)});
		}
		return encodings;
	}

	auto PreviewEngine::match_faces(const std::vector<EncodedFace> &encodings,
	                                std::vector<PreviewFaceResult> &faces) -> std::optional<bool> {
		bool matched = false;
		for (const auto &encoded : encodings) {
			auto match =
			    dependencies_.match_face(dependencies_.context, known_encodings_, encoded.encoding);
			if (match.accepted &&
			    (match.index < 0 || !std::cmp_less(match.index, known_encodings_.size()) ||
			     !std::cmp_less(match.index, known_model_count_) || !std::isfinite(match.score))) {
				return std::nullopt;
			}
			matched |= match.accepted;
			auto &face_result = faces[encoded.face_index];
			face_result.match = match;
			face_result.status =
			    match.accepted ? PreviewFaceStatus::kMatched : PreviewFaceStatus::kUnmatched;
			face_result.matching_attempted = true;
		}
		return matched;
	}

	auto PreviewEngine::process_gray_frame(cv::Mat gray_frame) -> PreviewFrameResult {
		if (!dependencies_valid()) {
			return {
			    .status        = PreviewFrameStatus::kInvalidDependencies,
			    .error_message = "Internal error: missing preview inference dependency",
			};
		}

		if (validate_frame(gray_frame, FrameChannelPolicy::kGray) !=
		    FrameValidationStatus::kValid) {
			return {
			    .status        = PreviewFrameStatus::kInvalidFrame,
			    .error_message = "Camera grayscale frame is invalid",
			};
		}

		apply_clahe_if_enabled(gray_frame, config_, clahe_);
		const auto brightness = measure_brightness(gray_frame);
		if (brightness.hist_total == 0.0 || brightness.darkness == kBrightnessPercentScale) {
			return {
			    .status     = PreviewFrameStatus::kBlackFrame,
			    .brightness = brightness,
			    .gray_frame = std::move(gray_frame),
			};
		}
		if (brightness.darkness > config_.dark_threshold) {
			return {
			    .status     = PreviewFrameStatus::kTooDark,
			    .brightness = brightness,
			    .gray_frame = std::move(gray_frame),
			};
		}

		const auto inference_start   = dependencies_.now(dependencies_.context);
		const auto inference_elapsed = [this, &inference_start]() -> std::chrono::milliseconds {
			return std::chrono::duration_cast<std::chrono::milliseconds>(
			    dependencies_.now(dependencies_.context) - inference_start);
		};

		auto prepared = dependencies_.prepare_frame(dependencies_.context, gray_frame);
		if (validate_frame(prepared, FrameChannelPolicy::kBgr) != FrameValidationStatus::kValid) {
			return {
			    .status         = PreviewFrameStatus::kDetectionFailed,
			    .brightness     = brightness,
			    .gray_frame     = std::move(gray_frame),
			    .error_message  = "Prepared frame for face detection is invalid",
			    .inference_time = inference_elapsed(),
			};
		}

		auto detection_result = dependencies_.detect_faces(dependencies_.context, prepared);
		if (!detection_result.ok()) {
			return {
			    .status         = PreviewFrameStatus::kDetectionFailed,
			    .brightness     = brightness,
			    .gray_frame     = std::move(gray_frame),
			    .error_message  = detection_result.error_message.empty()
			                          ? kFaceDetectionFailedMessage
			                          : std::move(detection_result.error_message),
			    .inference_time = inference_elapsed(),
			};
		}
		if (detection_result.detections.empty()) {
			return {
			    .status         = PreviewFrameStatus::kNoFace,
			    .brightness     = brightness,
			    .gray_frame     = std::move(gray_frame),
			    .inference_time = inference_elapsed(),
			};
		}

		std::vector<PreviewFaceResult> faces;
		faces.reserve(detection_result.detections.size());
		for (const auto &detection : detection_result.detections) {
			faces.push_back({.detection = detection});
		}
		if (!matching_enabled_) {
			return {
			    .status         = PreviewFrameStatus::kFacesDetected,
			    .brightness     = brightness,
			    .gray_frame     = std::move(gray_frame),
			    .faces          = std::move(faces),
			    .inference_time = inference_elapsed(),
			};
		}

		std::string first_encoding_error;
		auto        encodings =
		    encode_faces(prepared, detection_result.detections, faces, first_encoding_error);

		if (encodings.empty()) {
			return {
			    .status         = PreviewFrameStatus::kEncodingFailed,
			    .brightness     = brightness,
			    .gray_frame     = std::move(gray_frame),
			    .faces          = std::move(faces),
			    .error_message  = std::move(first_encoding_error),
			    .inference_time = inference_elapsed(),
			};
		}

		const auto matched = match_faces(encodings, faces);
		if (!matched.has_value()) {
			return {
			    .status         = PreviewFrameStatus::kInvalidMatchResult,
			    .brightness     = brightness,
			    .gray_frame     = std::move(gray_frame),
			    .error_message  = kFaceMatcherInvalidResultMessage,
			    .inference_time = inference_elapsed(),
			};
		}

		return {
		    .status =
		        *matched ? PreviewFrameStatus::kMatchedFace : PreviewFrameStatus::kUnmatchedFace,
		    .brightness     = brightness,
		    .gray_frame     = std::move(gray_frame),
		    .faces          = std::move(faces),
		    .inference_time = inference_elapsed(),
		};
	}

}  // namespace howdy::native
