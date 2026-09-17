#include "vision/preview_engine.hpp"

#include "config/runtime_config.hpp"
#include "vision/frame_validation.hpp"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <utility>

#include <opencv2/core.hpp>

namespace howdy::native {

	PreviewEngine::PreviewEngine(VideoConfig config, PreviewDependencies dependencies,
	                             std::vector<std::vector<float>> known_encodings,
	                             std::size_t known_model_count, bool matching_enabled)
	    : config_(std::move(config))
	    , clahe_(MakeClahe(config_))
	    , dependencies_(dependencies)
	    , known_encodings_(std::move(known_encodings))
	    , known_model_count_(known_model_count)
	    , matching_enabled_(matching_enabled) {}

	auto PreviewEngine::DependenciesValid() const -> bool {
		return dependencies_.now != nullptr &&
		       (matching_enabled_ ? dependencies_.inference.Complete()
		                          : dependencies_.inference.DetectionReady());
	}

	auto PreviewEngine::EncodeFaces(const cv::Mat                    &prepared,
	                                const std::vector<FaceDetection> &detections,
	                                std::vector<PreviewFaceResult>   &faces,
	                                std::string &first_error) const -> std::vector<EncodedFace> {
		std::vector<EncodedFace> encodings;
		encodings.reserve(detections.size());
		for (std::size_t index = 0; index < detections.size(); ++index) {
			auto encoding = dependencies_.inference.encode_face(dependencies_.inference.context,
			                                                    prepared, detections[index]);
			if (!encoding.Ok()) {
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

	auto PreviewEngine::MatchFaces(const std::vector<EncodedFace> &encodings,
	                               std::vector<PreviewFaceResult> &faces) -> std::optional<bool> {
		bool matched = false;
		for (const auto &encoded : encodings) {
			auto match = dependencies_.inference.match_face(dependencies_.inference.context,
			                                                known_encodings_, encoded.encoding);
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

	auto PreviewEngine::ProcessGrayFrame(cv::Mat gray_frame) -> PreviewFrameResult {
		if (!DependenciesValid()) {
			return {
			    .status        = PreviewFrameStatus::kInvalidDependencies,
			    .error_message = "Internal error: missing preview inference dependency",
			};
		}

		if (ValidateFrame(gray_frame, FrameChannelPolicy::kGray) != FrameValidationStatus::kValid) {
			return {
			    .status        = PreviewFrameStatus::kInvalidFrame,
			    .error_message = "Camera grayscale frame is invalid",
			};
		}

		ApplyClaheIfEnabled(gray_frame, config_, clahe_);
		const auto brightness = MeasureBrightness(gray_frame);
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

		const auto inference_start   = dependencies_.now(dependencies_.inference.context);
		const auto inference_elapsed = [this, &inference_start]() -> std::chrono::milliseconds {
			return std::chrono::duration_cast<std::chrono::milliseconds>(
			    dependencies_.now(dependencies_.inference.context) - inference_start);
		};

		auto prepared =
		    dependencies_.inference.prepare_frame(dependencies_.inference.context, gray_frame);
		if (ValidateFrame(prepared, FrameChannelPolicy::kBgr) != FrameValidationStatus::kValid) {
			return {
			    .status         = PreviewFrameStatus::kDetectionFailed,
			    .brightness     = brightness,
			    .gray_frame     = std::move(gray_frame),
			    .error_message  = "Prepared frame for face detection is invalid",
			    .inference_time = inference_elapsed(),
			};
		}

		auto detection_result =
		    dependencies_.inference.detect_faces(dependencies_.inference.context, prepared);
		if (!detection_result.Ok()) {
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
		    EncodeFaces(prepared, detection_result.detections, faces, first_encoding_error);

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

		const auto matched = MatchFaces(encodings, faces);
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
