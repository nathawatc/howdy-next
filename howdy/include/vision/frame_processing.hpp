#pragma once

#include "config/runtime_config.hpp"

#include <array>
#include <cstdint>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

namespace howdy::native {
	inline constexpr float kBrightnessPercentScale = 100.0F;

	struct BrightnessStats {
		double               hist_total = 0.0;
		float                darkness   = kBrightnessPercentScale;
		std::array<float, 8> bins_percent{};
	};

	enum class BrightnessDecision : std::uint8_t {
		kBlackFrame,
		kTooDark,
		kProcessFrame,
	};

	struct FrameGeometry {
		int width    = 0;
		int height   = 0;
		int rotation = 0;
	};

	auto MakeClahe(const VideoConfig &config) -> cv::Ptr<cv::CLAHE>;
	void ApplyClaheIfEnabled(cv::Mat &gray, const VideoConfig &config, cv::Ptr<cv::CLAHE> &clahe);
	auto RotateFrame(const cv::Mat &frame, const VideoConfig &config, int frame_number) -> cv::Mat;
	auto MeasureBrightness(const cv::Mat &gray) -> BrightnessStats;
	auto ClassifyBrightness(double hist_total, float darkness, float dark_threshold)
	    -> BrightnessDecision;
	auto FrameResizeScale(FrameGeometry frame, float max_height) -> double;

}  // namespace howdy::native
