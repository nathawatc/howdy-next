#pragma once

#include "config/runtime_config.hpp"
#include "vision/face_inference.hpp"
#include "vision/frame_processing.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

namespace howdy::native {

	enum class CompareFrameStatus : std::uint8_t {
		kReady,
		kBlackFrame,
		kTooDark,
		kInvalidInput,
		kInvalidPreprocessed,
	};

	struct CompareFrameResult {
		CompareFrameStatus status = CompareFrameStatus::kInvalidInput;
		BrightnessStats    brightness;
		cv::Mat            working_frame;
		std::string        error_message;
	};

	enum class CompareInferenceStatus : std::uint8_t {
		kNoMatch,
		kMatch,
		kInvalidPreparedFrame,
		kDetectionFailed,
		kEncodingFailed,
		kInvalidMatchResult,
		kInvalidDependencies,
	};

	struct CompareInferenceResult {
		CompareInferenceStatus status        = CompareInferenceStatus::kInvalidDependencies;
		int                    winning_index = -1;
		float                  winning_score = 0.0F;
		std::string            error_message;
	};

	class CompareEngine {
	public:
		explicit CompareEngine(const VideoConfig &config);
		CompareEngine(const VideoConfig &config, FaceInferenceOperations inference,
		              std::vector<std::vector<float>> known_encodings);

		// frame_number is one-based and preserves current rotation cadence.
		auto ProcessGrayFrame(cv::Mat gray_frame, int frame_number) -> CompareFrameResult;

		auto ProcessFaceFrame(const cv::Mat &working_frame) -> CompareInferenceResult;

	private:
		VideoConfig        config_;
		cv::Ptr<cv::CLAHE> clahe_;

		FaceInferenceOperations         inference_;
		std::vector<std::vector<float>> known_encodings_;
	};

}  // namespace howdy::native
