#pragma once

#include "config/runtime_config.hpp"
#include "vision/face_inference.hpp"
#include "vision/face_matching.hpp"
#include "vision/frame_processing.hpp"

#include <cstdint>
#include <functional>
#include <string>

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
		explicit CompareEngine(VideoConfig config);
		CompareEngine(VideoConfig config, FaceInferenceOperations inference,
		              PreparedFaceMatcher matcher, FaceMatchingOperations matching = {});

		// frame_number is one-based and preserves current rotation cadence.
		auto ProcessGrayFrame(cv::Mat gray_frame, int frame_number) -> CompareFrameResult;

		auto ProcessFaceFrame(const cv::Mat         &working_frame,
		                      std::function<void()> *before_first_encode = nullptr)
		    -> CompareInferenceResult;

	private:
		VideoConfig        config_;
		cv::Ptr<cv::CLAHE> clahe_;

		FaceInferenceOperations inference_;
		PreparedFaceMatcher     matcher_;
		FaceMatchingOperations  matching_;
	};

}  // namespace howdy::native
