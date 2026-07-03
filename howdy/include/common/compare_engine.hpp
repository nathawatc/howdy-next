#pragma once

#include "common/frame_processing.hpp"
#include "config/runtime_config.hpp"

#include <string>

#include <opencv2/core.hpp>

namespace howdy::native {

	enum class CompareFrameStatus {
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

	class CompareEngine {
	public:
		explicit CompareEngine(const VideoConfig &config);

		// frame_number is one-based and preserves current rotation cadence.
		auto process_gray_frame(cv::Mat gray_frame, int frame_number) -> CompareFrameResult;

	private:
		VideoConfig        config_;
		cv::Ptr<cv::CLAHE> clahe_;
	};

}  // namespace howdy::native
