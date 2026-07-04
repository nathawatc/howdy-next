#include "common/compare_engine.hpp"

#include "common/compare_logic.hpp"
#include "common/frame_validation.hpp"

#include <cmath>
#include <string>
#include <utility>

#include <opencv2/imgproc.hpp>

namespace {

	auto apply_rotation(const cv::Mat &frame, int rotate, int frame_number) -> cv::Mat {
		if (rotate == 1) {
			if (frame_number % 3 == 1) {
				cv::Mat rotated;
				cv::rotate(frame, rotated, cv::ROTATE_90_COUNTERCLOCKWISE);
				return rotated;
			}
			if (frame_number % 3 == 2) {
				cv::Mat rotated;
				cv::rotate(frame, rotated, cv::ROTATE_90_CLOCKWISE);
				return rotated;
			}
		} else if (rotate == 2) {
			cv::Mat rotated;
			cv::rotate(frame, rotated,
			           frame_number % 2 == 0 ? cv::ROTATE_90_COUNTERCLOCKWISE
			                                 : cv::ROTATE_90_CLOCKWISE);
			return rotated;
		}

		return frame;
	}

	auto frame_validation_message(const std::string &subject, const cv::Mat &frame,
	                              howdy::native::FrameValidationStatus status) -> std::string {
		switch (status) {
			case howdy::native::FrameValidationStatus::kValid:
				return {};
			case howdy::native::FrameValidationStatus::kEmpty:
				return subject + " is empty";
			case howdy::native::FrameValidationStatus::kUnsupportedDimensions:
				return subject + " has unsupported frame dimensions: " + std::to_string(frame.dims);
			case howdy::native::FrameValidationStatus::kOversizedDimensions:
				return subject + " has oversized frame dimensions: " + std::to_string(frame.cols) +
				       "x" + std::to_string(frame.rows) + " (max supported dimension: " +
				       std::to_string(howdy::native::kMaxFrameDimension) + ")";
			case howdy::native::FrameValidationStatus::kUnsupportedChannelCount:
				return subject +
				       " has unsupported channel count: " + std::to_string(frame.channels());
			case howdy::native::FrameValidationStatus::kUnsupportedPixelType:
				return subject + " has unsupported pixel type: " + std::to_string(frame.type());
		}
		return subject + " is invalid";
	}

	auto prepared_frame_validation_message(const cv::Mat                       &frame,
	                                       howdy::native::FrameValidationStatus status)
	    -> std::string {
		const std::string subject = "Prepared frame for face detection";
		switch (status) {
			case howdy::native::FrameValidationStatus::kValid:
				return {};
			case howdy::native::FrameValidationStatus::kEmpty:
				return subject + " is empty";
			case howdy::native::FrameValidationStatus::kUnsupportedDimensions:
				return subject + " has unsupported frame dimensions: " + std::to_string(frame.dims);
			case howdy::native::FrameValidationStatus::kOversizedDimensions:
				return subject + " has oversized frame dimensions: " + std::to_string(frame.cols) +
				       "x" + std::to_string(frame.rows) + " (max supported dimension: " +
				       std::to_string(howdy::native::kMaxFrameDimension) + ")";
			case howdy::native::FrameValidationStatus::kUnsupportedChannelCount:
				return subject +
				       " has unsupported channel count: " + std::to_string(frame.channels());
			case howdy::native::FrameValidationStatus::kUnsupportedPixelType:
				return subject + " has unsupported pixel type: " + std::to_string(frame.type());
		}
		return subject + " is invalid";
	}

}  // namespace

namespace howdy::native {

	// Public API accepts a const reference; engine intentionally owns a config copy.
	// NOLINTNEXTLINE(modernize-pass-by-value)
	CompareEngine::CompareEngine(const VideoConfig &config)
	    : config_(config)
	    , clahe_(make_clahe(config_)) {}

	// Public API accepts a const reference; engine intentionally owns a config copy.
	// NOLINTNEXTLINE(modernize-pass-by-value)
	CompareEngine::CompareEngine(const VideoConfig              &config,
	                             CompareInferenceDependencies    inference_dependencies,
	                             std::vector<std::vector<float>> known_encodings)
	    : config_(config)
	    , clahe_(make_clahe(config_))
	    , inference_dependencies_(inference_dependencies)
	    , known_encodings_(std::move(known_encodings)) {}

	auto CompareEngine::process_gray_frame(cv::Mat gray_frame, int frame_number)
	    -> CompareFrameResult {
		const auto input_validation = validate_frame(gray_frame, FrameChannelPolicy::kGray);
		if (input_validation != FrameValidationStatus::kValid) {
			return {
			    .status        = CompareFrameStatus::kInvalidInput,
			    .error_message = frame_validation_message("Camera grayscale frame", gray_frame,
			                                              input_validation),
			};
		}

		apply_clahe_if_enabled(gray_frame, config_, clahe_);

		const auto brightness = measure_brightness(gray_frame);
		switch (classify_brightness(brightness.hist_total, brightness.darkness,
		                            config_.dark_threshold)) {
			case BrightnessDecision::kBlackFrame:
				return {
				    .status     = CompareFrameStatus::kBlackFrame,
				    .brightness = brightness,
				};
			case BrightnessDecision::kTooDark:
				return {
				    .status     = CompareFrameStatus::kTooDark,
				    .brightness = brightness,
				};
			case BrightnessDecision::kProcessFrame:
				break;
		}

		cv::Mat working_frame = gray_frame;

		const double scaling_factor = compare_resize_scale(gray_frame.cols, gray_frame.rows,
		                                                   config_.rotate, config_.max_height);

		if (scaling_factor < 1.0) {
			cv::resize(gray_frame, working_frame, cv::Size(), scaling_factor, scaling_factor,
			           cv::INTER_AREA);
		}

		working_frame = apply_rotation(working_frame, config_.rotate, frame_number);

		const auto working_validation = validate_frame(working_frame, FrameChannelPolicy::kGray);
		if (working_validation != FrameValidationStatus::kValid) {
			return {
			    .status        = CompareFrameStatus::kInvalidPreprocessed,
			    .brightness    = brightness,
			    .error_message = frame_validation_message("Frame after preprocessing",
			                                              working_frame, working_validation),
			};
		}

		return {
		    .status        = CompareFrameStatus::kReady,
		    .brightness    = brightness,
		    .working_frame = std::move(working_frame),
		};
	}

	auto CompareEngine::process_face_frame(const cv::Mat &working_frame) -> CompareInferenceResult {
		if (inference_dependencies_.context == nullptr ||
		    inference_dependencies_.prepare_face_frame == nullptr ||
		    inference_dependencies_.detect_faces == nullptr ||
		    inference_dependencies_.encode_face == nullptr ||
		    inference_dependencies_.find_best_match == nullptr) {
			return {
			    .status = CompareInferenceStatus::kInvalidDependencies,
			};
		}

		const auto prepared = inference_dependencies_.prepare_face_frame(
		    inference_dependencies_.context, working_frame);
		const auto prepared_validation = validate_frame(prepared, FrameChannelPolicy::kBgr);
		if (prepared_validation != FrameValidationStatus::kValid) {
			return {
			    .status        = CompareInferenceStatus::kInvalidPreparedFrame,
			    .error_message = prepared_frame_validation_message(prepared, prepared_validation),
			};
		}

		const auto detection_result =
		    inference_dependencies_.detect_faces(inference_dependencies_.context, prepared);
		if (!detection_result.ok()) {
			return {
			    .status        = CompareInferenceStatus::kDetectionFailed,
			    .error_message = detection_result.error_message,
			};
		}

		std::string first_encoding_error;
		bool        reached_matching = false;
		for (const auto &face : detection_result.detections) {
			const auto encoding_result = inference_dependencies_.encode_face(
			    inference_dependencies_.context, prepared, face);
			if (!encoding_result.ok()) {
				if (first_encoding_error.empty()) {
					first_encoding_error = encoding_result.error_message.empty()
					                           ? "Face encoding returned invalid embedding"
					                           : encoding_result.error_message;
				}
				continue;
			}
			reached_matching = true;
			const auto match = inference_dependencies_.find_best_match(
			    inference_dependencies_.context, known_encodings_, encoding_result.encoding);
			if (match.accepted) {
				if (match.index < 0 || !std::cmp_less(match.index, known_encodings_.size()) ||
				    !std::isfinite(match.score)) {
					return {
					    .status        = CompareInferenceStatus::kInvalidMatchResult,
					    .error_message = "Face matcher returned invalid match result",
					};
				}
				return {
				    .status        = CompareInferenceStatus::kMatch,
				    .winning_index = match.index,
				    .winning_score = match.score,
				};
			}
		}
		if (!reached_matching && !first_encoding_error.empty()) {
			return {
			    .status        = CompareInferenceStatus::kEncodingFailed,
			    .error_message = std::move(first_encoding_error),
			};
		}

		return {
		    .status = CompareInferenceStatus::kNoMatch,
		};
	}

}  // namespace howdy::native
