#pragma once

#include <opencv2/core.hpp>

namespace howdy::native {

	inline constexpr int kMaxFrameDimension = 8192;

	enum class FrameValidationStatus {
		kValid,
		kEmpty,
		kUnsupportedDimensions,
		kOversizedDimensions,
		kUnsupportedChannelCount,
		kUnsupportedPixelType,
	};

	enum class FrameChannelPolicy {
		kCameraInput,
		kGray,
		kBgr,
	};

	[[nodiscard]] inline auto is_supported_frame_channel_count(int                channels,
	                                                           FrameChannelPolicy policy) -> bool {
		switch (policy) {
			case FrameChannelPolicy::kCameraInput:
				return channels == 1 || channels == 3 || channels == 4;
			case FrameChannelPolicy::kGray:
				return channels == 1;
			case FrameChannelPolicy::kBgr:
				return channels == 3;
		}
		return false;
	}

	[[nodiscard]] inline auto validate_frame(const cv::Mat     &frame,
	                                         FrameChannelPolicy channel_policy)
	    -> FrameValidationStatus {
		if (frame.empty()) {
			return FrameValidationStatus::kEmpty;
		}

		if (frame.dims != 2) {
			return FrameValidationStatus::kUnsupportedDimensions;
		}
		if (frame.rows > kMaxFrameDimension || frame.cols > kMaxFrameDimension) {
			return FrameValidationStatus::kOversizedDimensions;
		}
		if (!is_supported_frame_channel_count(frame.channels(), channel_policy)) {
			return FrameValidationStatus::kUnsupportedChannelCount;
		}
		if (frame.depth() != CV_8U) {
			return FrameValidationStatus::kUnsupportedPixelType;
		}
		return FrameValidationStatus::kValid;
	}

}  // namespace howdy::native
