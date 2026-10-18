#include "compare/engine.hpp"

#include "config/runtime_config.hpp"
#include "support/frame_limits.hpp"
#include "vision/face_detection.hpp"
#include "vision/face_encoding.hpp"
#include "vision/face_inference.hpp"
#include "vision/face_matching.hpp"
#include "vision/frame_processing.hpp"
#include "vision/frame_validation.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <utility>

#include <opencv2/core/fast_math.hpp>
#include <opencv2/core/mat.hpp>
#include <opencv2/core/types.hpp>
#include <opencv2/imgproc.hpp>

namespace {

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
	                             PreparedFaceMatcher matcher, FaceMatchingOperations matching)
	    : config_(std::move(config))
	    , clahe_(MakeClahe(config_))
	    , inference_(inference)
	    , matcher_(std::move(matcher))
	    , matching_(matching) {}

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

		const double scaling_factor = FrameResizeScale(
		    {.width = gray_frame.cols, .height = gray_frame.rows, .rotation = config_.rotate},
		    config_.max_height);
		const cv::Mat source_frame  = std::move(gray_frame);
		cv::Mat       working_frame = source_frame;
		if (scaling_factor < 1.0) {
			const cv::Size resized_size(
			    std::max(1, cvRound(static_cast<double>(source_frame.cols) * scaling_factor)),
			    std::max(1, cvRound(static_cast<double>(source_frame.rows) * scaling_factor)));
			cv::resize(source_frame, working_frame, resized_size, 0.0, 0.0, cv::INTER_AREA);
		}
		ApplyClaheIfEnabled(working_frame, config_, clahe_);

		const auto brightness = MeasureBrightness(working_frame);
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

		working_frame = RotateFrame(working_frame, config_, frame_number);

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

	auto CompareEngine::ProcessFaceFrame(const cv::Mat         &working_frame,
	                                     std::function<void()> *before_first_encode)
	    -> CompareInferenceResult {
		if (!FaceInferenceComplete(inference_) || matching_.match_face == nullptr) {
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
		if (!FaceDetectionSucceeded(detection_result)) {
			return {
			    .status        = CompareInferenceStatus::kDetectionFailed,
			    .error_message = detection_result.error_message,
			};
		}

		std::string first_encoding_error;
		bool        reached_matching = false;
		for (const auto &face : detection_result.detections) {
			if (before_first_encode != nullptr && *before_first_encode) {
				(*before_first_encode)();
				*before_first_encode = {};
			}
			const auto encoding_result = inference_.encode_face(inference_.context, prepared, face);
			if (!FaceEncodingSucceeded(encoding_result)) {
				if (first_encoding_error.empty()) {
					first_encoding_error = encoding_result.error_message.empty()
					                           ? kInvalidFaceEncodingMessage
					                           : encoding_result.error_message;
				}
				continue;
			}
			reached_matching = true;
			const auto match =
			    matching_.match_face(matching_.context, matcher_, encoding_result.encoding);
			if (match.accepted) {
				if (match.index < 0 || !std::cmp_less(match.index, matcher_.Candidates().size()) ||
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
