#pragma once

#include "config/runtime_config.hpp"
#include "vision/face_detection.hpp"
#include "vision/face_encoding.hpp"
#include "vision/face_matching.hpp"
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

	using PrepareFaceFrameFn = cv::Mat (*)(void *context, const cv::Mat &frame);

	using DetectFacesFn = FaceDetectionResult (*)(void *context, const cv::Mat &frame);

	using EncodeFaceFn = FaceEncodingResult (*)(void *context, const cv::Mat &frame,
	                                            const FaceDetection &face);

	using FindBestMatchFn = FaceMatch (*)(void                                  *context,
	                                      const std::vector<std::vector<float>> &known,
	                                      const std::vector<float>              &probe);

	struct CompareInferenceDependencies {
		void              *context            = nullptr;
		PrepareFaceFrameFn prepare_face_frame = nullptr;
		DetectFacesFn      detect_faces       = nullptr;
		EncodeFaceFn       encode_face        = nullptr;
		FindBestMatchFn    find_best_match    = nullptr;
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
		CompareEngine(const VideoConfig              &config,
		              CompareInferenceDependencies    inference_dependencies,
		              std::vector<std::vector<float>> known_encodings);

		// frame_number is one-based and preserves current rotation cadence.
		auto process_gray_frame(cv::Mat gray_frame, int frame_number) -> CompareFrameResult;

		auto process_face_frame(const cv::Mat &working_frame) -> CompareInferenceResult;

	private:
		VideoConfig        config_;
		cv::Ptr<cv::CLAHE> clahe_;

		CompareInferenceDependencies    inference_dependencies_;
		std::vector<std::vector<float>> known_encodings_;
	};

}  // namespace howdy::native
