#pragma once

#include "protocol/compare_exit.hpp"
#include "support/face_metric.hpp"

#include <cstdint>
#include <exception>
#include <iosfwd>
#include <string_view>

#include <opencv2/core.hpp>

namespace howdy::native {
	inline constexpr auto kAllFramesTooDarkMessage =
	    "All frames were too dark; check dark_threshold";
	inline constexpr auto kAverageDarknessLabel = "Average darkness: ";
	inline constexpr auto kThresholdLabel       = ", Threshold: ";

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

	auto UpdateBestScore(float current, float score, FaceMetric metric) -> float;

	auto ClassifyBrightness(double hist_total, float darkness, float dark_threshold)
	    -> BrightnessDecision;

	auto TimeoutExit(int dark_tries, int valid_frames) -> CompareExit;
	auto CompareResizeScale(FrameGeometry frame, float max_height) -> double;
	auto CompareAbortFromCvException(const cv::Exception &error, std::ostream &stream,
	                                 std::string_view context) -> CompareExit;
	auto CompareAbortFromException(const std::exception &error, std::ostream &stream,
	                               std::string_view context) -> CompareExit;
	auto CompareAbortFromUnknownException(std::ostream &stream, std::string_view context)
	    -> CompareExit;

}  // namespace howdy::native
