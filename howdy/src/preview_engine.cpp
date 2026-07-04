#include "common/preview_engine.hpp"

#include "common/frame_validation.hpp"

#include <chrono>
#include <cmath>
#include <utility>

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

	auto PreviewEngine::process_gray_frame(cv::Mat gray_frame) -> PreviewFrameResult {
		if (dependencies_.context == nullptr || dependencies_.prepare_frame == nullptr ||
		    dependencies_.detect_faces == nullptr || dependencies_.now == nullptr ||
		    (matching_enabled_ &&
		     (dependencies_.encode_face == nullptr || dependencies_.match_face == nullptr))) {
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
		if (brightness.hist_total == 0.0 || brightness.darkness == 100.0F) {
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

		const auto recognition_start   = dependencies_.now(dependencies_.context);
		const auto recognition_elapsed = [this, &recognition_start]() {
			return std::chrono::duration_cast<std::chrono::milliseconds>(
			    dependencies_.now(dependencies_.context) - recognition_start);
		};

		auto prepared = dependencies_.prepare_frame(dependencies_.context, gray_frame);
		if (validate_frame(prepared, FrameChannelPolicy::kBgr) != FrameValidationStatus::kValid) {
			return {
			    .status           = PreviewFrameStatus::kDetectionFailed,
			    .brightness       = brightness,
			    .gray_frame       = std::move(gray_frame),
			    .error_message    = "Prepared frame for face detection is invalid",
			    .recognition_time = recognition_elapsed(),
			};
		}

		auto detection_result = dependencies_.detect_faces(dependencies_.context, prepared);
		if (!detection_result.ok()) {
			return {
			    .status           = PreviewFrameStatus::kDetectionFailed,
			    .brightness       = brightness,
			    .gray_frame       = std::move(gray_frame),
			    .error_message    = detection_result.error_message.empty()
			                            ? "Face detection failed"
			                            : std::move(detection_result.error_message),
			    .recognition_time = recognition_elapsed(),
			};
		}
		if (detection_result.detections.empty()) {
			return {
			    .status           = PreviewFrameStatus::kNoFace,
			    .brightness       = brightness,
			    .gray_frame       = std::move(gray_frame),
			    .recognition_time = recognition_elapsed(),
			};
		}

		std::vector<PreviewFaceResult> faces;
		faces.reserve(detection_result.detections.size());
		for (const auto &detection : detection_result.detections) {
			faces.push_back({.detection = detection});
		}
		if (!matching_enabled_) {
			return {
			    .status           = PreviewFrameStatus::kFacesDetected,
			    .brightness       = brightness,
			    .gray_frame       = std::move(gray_frame),
			    .faces            = std::move(faces),
			    .recognition_time = recognition_elapsed(),
			};
		}

		struct EncodedFace {
			std::size_t        face_index = 0;
			std::vector<float> encoding;
		};

		std::vector<EncodedFace> encodings;
		encodings.reserve(detection_result.detections.size());
		std::string first_encoding_error;
		for (std::size_t index = 0; index < detection_result.detections.size(); ++index) {
			const auto &detection = detection_result.detections[index];
			auto encoding = dependencies_.encode_face(dependencies_.context, prepared, detection);
			if (!encoding.ok()) {
				faces[index].status = PreviewFaceStatus::kEncodingFailed;
				if (first_encoding_error.empty()) {
					first_encoding_error = encoding.error_message.empty()
					                           ? "Face encoding returned invalid embedding"
					                           : std::move(encoding.error_message);
				}
				continue;
			}
			encodings.push_back({.face_index = index, .encoding = std::move(encoding.encoding)});
		}

		if (encodings.empty()) {
			return {
			    .status           = PreviewFrameStatus::kEncodingFailed,
			    .brightness       = brightness,
			    .gray_frame       = std::move(gray_frame),
			    .faces            = std::move(faces),
			    .error_message    = std::move(first_encoding_error),
			    .recognition_time = recognition_elapsed(),
			};
		}

		bool matched = false;
		for (const auto &encoded : encodings) {
			auto match =
			    dependencies_.match_face(dependencies_.context, known_encodings_, encoded.encoding);
			if (match.accepted &&
			    (match.index < 0 || !std::cmp_less(match.index, known_encodings_.size()) ||
			     !std::cmp_less(match.index, known_model_count_) || !std::isfinite(match.score))) {
				return {
				    .status           = PreviewFrameStatus::kInvalidMatchResult,
				    .brightness       = brightness,
				    .gray_frame       = std::move(gray_frame),
				    .error_message    = "Face matcher returned invalid match result",
				    .recognition_time = recognition_elapsed(),
				};
			}
			matched |= match.accepted;
			auto &face_result = faces[encoded.face_index];
			face_result.match = match;
			face_result.status =
			    match.accepted ? PreviewFaceStatus::kMatched : PreviewFaceStatus::kUnmatched;
			face_result.matching_attempted = true;
		}

		return {
		    .status =
		        matched ? PreviewFrameStatus::kMatchedFace : PreviewFrameStatus::kUnmatchedFace,
		    .brightness       = brightness,
		    .gray_frame       = std::move(gray_frame),
		    .faces            = std::move(faces),
		    .recognition_time = recognition_elapsed(),
		};
	}

}  // namespace howdy::native
