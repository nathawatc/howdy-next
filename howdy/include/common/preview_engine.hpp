#pragma once

#include "common/frame_processing.hpp"
#include "config/runtime_config.hpp"
#include "core/face_detection.hpp"
#include "core/face_encoding.hpp"
#include "core/face_matching.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

namespace howdy::native {

	using PreparePreviewFrameFn = cv::Mat (*)(void *context, const cv::Mat &frame);
	using DetectPreviewFacesFn  = FaceDetectionResult (*)(void *context, const cv::Mat &frame);
	using EncodePreviewFaceFn   = FaceEncodingResult (*)(void *context, const cv::Mat &frame,
	                                                     const FaceDetection &face);
	using MatchPreviewFaceFn    = FaceMatch (*)(void                                  *context,
	                                            const std::vector<std::vector<float>> &known,
	                                            const std::vector<float>              &probe);
	using PreviewNowFn          = std::chrono::steady_clock::time_point (*)(void *context);

	inline auto preview_steady_clock_now(void *context) -> std::chrono::steady_clock::time_point {
		(void)context;
		return std::chrono::steady_clock::now();
	}

	struct PreviewInferenceDependencies {
		void                 *context       = nullptr;
		PreparePreviewFrameFn prepare_frame = nullptr;
		DetectPreviewFacesFn  detect_faces  = nullptr;
		EncodePreviewFaceFn   encode_face   = nullptr;
		MatchPreviewFaceFn    match_face    = nullptr;
		PreviewNowFn          now           = preview_steady_clock_now;
	};

	enum class PreviewFrameStatus : std::uint8_t {
		kNoFace,
		kFacesDetected,
		kUnmatchedFace,
		kMatchedFace,
		kBlackFrame,
		kTooDark,
		kInvalidFrame,
		kDetectionFailed,
		kEncodingFailed,
		kInvalidMatchResult,
		kInvalidDependencies,
	};

	enum class PreviewFaceStatus : std::uint8_t {
		kDetected,
		kEncodingFailed,
		kUnmatched,
		kMatched,
	};

	struct PreviewFaceResult {
		FaceDetection     detection;
		FaceMatch         match;
		PreviewFaceStatus status             = PreviewFaceStatus::kDetected;
		bool              matching_attempted = false;
	};

	struct PreviewFrameResult {
		PreviewFrameStatus             status = PreviewFrameStatus::kInvalidDependencies;
		BrightnessStats                brightness;
		cv::Mat                        gray_frame;
		std::vector<PreviewFaceResult> faces;
		std::string                    error_message;
		std::chrono::milliseconds      inference_time{0};
	};

	class PreviewEngine {
	public:
		PreviewEngine(VideoConfig config, PreviewInferenceDependencies dependencies,
		              std::vector<std::vector<float>> known_encodings,
		              std::size_t known_model_count, bool matching_enabled);

		auto process_gray_frame(cv::Mat gray_frame) -> PreviewFrameResult;

	private:
		struct EncodedFace {
			std::size_t        face_index = 0;
			std::vector<float> encoding;
		};

		[[nodiscard]] auto dependencies_valid() const -> bool;
		auto encode_faces(const cv::Mat &prepared, const std::vector<FaceDetection> &detections,
		                  std::vector<PreviewFaceResult> &faces, std::string &first_error) const
		    -> std::vector<EncodedFace>;
		auto match_faces(const std::vector<EncodedFace> &encodings,
		                 std::vector<PreviewFaceResult> &faces) -> std::optional<bool>;

		VideoConfig                     config_;
		cv::Ptr<cv::CLAHE>              clahe_;
		PreviewInferenceDependencies    dependencies_;
		std::vector<std::vector<float>> known_encodings_;
		std::size_t                     known_model_count_ = 0;
		bool                            matching_enabled_  = false;
	};

}  // namespace howdy::native
