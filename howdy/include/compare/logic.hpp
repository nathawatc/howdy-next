#pragma once

#include "protocol/compare_exit.hpp"

#include <cstdint>
#include <exception>
#include <iosfwd>
#include <string>
#include <string_view>

#include <opencv2/core.hpp>

namespace howdy::native {

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

	auto update_best_score(float current, float score, const std::string &metric) -> float;

	auto classify_brightness(double hist_total, float darkness, float dark_threshold)
	    -> BrightnessDecision;

	auto timeout_exit(int dark_tries, int valid_frames) -> CompareExit;
	auto compare_resize_scale(FrameGeometry frame, float max_height) -> double;
	auto compare_abort_from_cv_exception(const cv::Exception &error, std::ostream &stream,
	                                     std::string_view context) -> CompareExit;
	auto compare_abort_from_exception(const std::exception &error, std::ostream &stream,
	                                  std::string_view context) -> CompareExit;
	auto compare_abort_from_unknown_exception(std::ostream &stream, std::string_view context)
	    -> CompareExit;

}  // namespace howdy::native
