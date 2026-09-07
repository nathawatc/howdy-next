#pragma once

#include "config/runtime_config.hpp"

#include <array>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

namespace howdy::native {
	inline constexpr float kBrightnessPercentScale = 100.0F;

	struct BrightnessStats {
		double               hist_total = 0.0;
		float                darkness   = kBrightnessPercentScale;
		std::array<float, 8> bins_percent{};
	};

	auto MakeClahe(const VideoConfig &config) -> cv::Ptr<cv::CLAHE>;
	void ApplyClaheIfEnabled(cv::Mat &gray, const VideoConfig &config, cv::Ptr<cv::CLAHE> &clahe);
	auto MeasureBrightness(const cv::Mat &gray) -> BrightnessStats;

}  // namespace howdy::native
