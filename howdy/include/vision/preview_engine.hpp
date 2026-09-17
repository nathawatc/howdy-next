#pragma once

#include "config/runtime_config.hpp"
#include "vision/face_detection.hpp"
#include "vision/face_inference.hpp"
#include "vision/face_matching.hpp"
#include "vision/frame_processing.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

namespace howdy::native {

	using PreviewNowFn = std::chrono::steady_clock::time_point (*)(void *context);

	inline auto PreviewSteadyClockNow(void *context) -> std::chrono::steady_clock::time_point {
		(void)context;
		return std::chrono::steady_clock::now();
	}

	struct PreviewDependencies {
		FaceInferenceOperations inference;
		PreviewNowFn            now = PreviewSteadyClockNow;
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
		PreviewEngine(VideoConfig config, PreviewDependencies dependencies,
		              std::vector<std::vector<float>> known_encodings,
		              std::size_t known_model_count, bool matching_enabled);

		auto ProcessGrayFrame(cv::Mat gray_frame) -> PreviewFrameResult;

	private:
		struct EncodedFace {
			std::size_t        face_index = 0;
			std::vector<float> encoding;
		};

		[[nodiscard]] auto DependenciesValid() const -> bool;
		auto EncodeFaces(const cv::Mat &prepared, const std::vector<FaceDetection> &detections,
		                 std::vector<PreviewFaceResult> &faces, std::string &first_error) const
		    -> std::vector<EncodedFace>;
		auto MatchFaces(const std::vector<EncodedFace> &encodings,
		                std::vector<PreviewFaceResult> &faces) -> std::optional<bool>;

		VideoConfig                     config_;
		cv::Ptr<cv::CLAHE>              clahe_;
		PreviewDependencies             dependencies_;
		std::vector<std::vector<float>> known_encodings_;
		std::size_t                     known_model_count_ = 0;
		bool                            matching_enabled_  = false;
	};

}  // namespace howdy::native
