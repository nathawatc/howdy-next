#include "compare/engine.hpp"

#include "compare/logic.hpp"
#include "config/runtime_config.hpp"
#include "vision/frame_validation.hpp"

#include <cmath>
#include <string>
#include <utility>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

namespace {

	struct RotationState {
		int rotation     = 0;
		int frame_number = 0;
	};

	auto ApplyRotation(const cv::Mat &frame, RotationState state) -> cv::Mat {
		if (state.rotation == 1) {
			if (state.frame_number % 3 == 1) {
				cv::Mat rotated;
				cv::rotate(frame, rotated, cv::ROTATE_90_COUNTERCLOCKWISE);
				return rotated;
			}
			if (state.frame_number % 3 == 2) {
				cv::Mat rotated;
				cv::rotate(frame, rotated, cv::ROTATE_90_CLOCKWISE);
				return rotated;
			}
		} else if (state.rotation == 2) {
			cv::Mat rotated;
			cv::rotate(frame, rotated,
			           state.frame_number % 2 == 0 ? cv::ROTATE_90_COUNTERCLOCKWISE
			                                       : cv::ROTATE_90_CLOCKWISE);
			return rotated;
		}

		return frame;
	}

	auto FrameValidationMessage(const std::string &subject, const cv::Mat &frame,
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

}  // namespace

namespace howdy::native {

	CompareEngine::CompareEngine(VideoConfig config)
	    : config_(std::move(config))
	    , clahe_(MakeClahe(config_)) {}

	CompareEngine::CompareEngine(VideoConfig config, FaceInferenceOperations inference,
	                             std::vector<std::vector<float>> known_encodings)
	    : config_(std::move(config))
	    , clahe_(MakeClahe(config_))
	    , inference_(inference)
	    , known_encodings_(std::move(known_encodings)) {}

	auto CompareEngine::ProcessGrayFrame(cv::Mat gray_frame, int frame_number)
	    -> CompareFrameResult {
		const auto input_validation = ValidateFrame(gray_frame, FrameChannelPolicy::kGray);
		if (input_validation != FrameValidationStatus::kValid) {
			return {
			    .status = CompareFrameStatus::kInvalidInput,
			    .error_message =
			        FrameValidationMessage("Camera grayscale frame", gray_frame, input_validation),
			};
		}

		ApplyClaheIfEnabled(gray_frame, config_, clahe_);

		const auto brightness = MeasureBrightness(gray_frame);
		switch (ClassifyBrightness(brightness.hist_total, brightness.darkness,
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

		const double scaling_factor = CompareResizeScale(
		    {.width = gray_frame.cols, .height = gray_frame.rows, .rotation = config_.rotate},
		    config_.max_height);

		if (scaling_factor < 1.0) {
			cv::resize(gray_frame, working_frame, cv::Size(), scaling_factor, scaling_factor,
			           cv::INTER_AREA);
		}

		working_frame = ApplyRotation(working_frame,
		                              {.rotation = config_.rotate, .frame_number = frame_number});

		const auto working_validation = ValidateFrame(working_frame, FrameChannelPolicy::kGray);
		if (working_validation != FrameValidationStatus::kValid) {
			return {
			    .status        = CompareFrameStatus::kInvalidPreprocessed,
			    .brightness    = brightness,
			    .error_message = FrameValidationMessage("Frame after preprocessing", working_frame,
			                                            working_validation),
			};
		}

		return {
		    .status        = CompareFrameStatus::kReady,
		    .brightness    = brightness,
		    .working_frame = std::move(working_frame),
		};
	}

	auto CompareEngine::ProcessFaceFrame(const cv::Mat &working_frame) -> CompareInferenceResult {
		if (!inference_.Complete()) {
			return {
			    .status = CompareInferenceStatus::kInvalidDependencies,
			};
		}

		const auto prepared = inference_.prepare_frame(inference_.context, working_frame);
		const auto prepared_validation = ValidateFrame(prepared, FrameChannelPolicy::kBgr);
		if (prepared_validation != FrameValidationStatus::kValid) {
			return {
			    .status        = CompareInferenceStatus::kInvalidPreparedFrame,
			    .error_message = FrameValidationMessage("Prepared frame for face detection",
			                                            prepared, prepared_validation),
			};
		}

		const auto detection_result = inference_.detect_faces(inference_.context, prepared);
		if (!detection_result.Ok()) {
			return {
			    .status        = CompareInferenceStatus::kDetectionFailed,
			    .error_message = detection_result.error_message,
			};
		}

		std::string first_encoding_error;
		bool        reached_matching = false;
		for (const auto &face : detection_result.detections) {
			const auto encoding_result = inference_.encode_face(inference_.context, prepared, face);
			if (!encoding_result.Ok()) {
				if (first_encoding_error.empty()) {
					first_encoding_error = encoding_result.error_message.empty()
					                           ? kInvalidFaceEncodingMessage
					                           : encoding_result.error_message;
				}
				continue;
			}
			reached_matching = true;
			const auto match = inference_.match_face(inference_.context, known_encodings_,
			                                         encoding_result.encoding);
			if (match.accepted) {
				if (match.index < 0 || !std::cmp_less(match.index, known_encodings_.size()) ||
				    !std::isfinite(match.score)) {
					return {
					    .status        = CompareInferenceStatus::kInvalidMatchResult,
					    .error_message = kFaceMatcherInvalidResultMessage,
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
