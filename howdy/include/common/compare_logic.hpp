#pragma once

#include "common/compare_exit.hpp"

#include <exception>
#include <iosfwd>
#include <string>
#include <string_view>

#include <opencv2/core.hpp>

namespace howdy::native {

	enum class BrightnessDecision {
		kBlackFrame,
		kTooDark,
		kProcessFrame,
	};

	auto update_best_score(float current, float score, const std::string &metric) -> float;

	auto classify_brightness(double hist_total, float darkness, float dark_threshold)
	    -> BrightnessDecision;

	auto timeout_exit(int dark_tries, int valid_frames) -> CompareExit;
	auto compare_resize_scale(int frame_width, int frame_height, int rotate, float max_height)
	    -> double;
	auto compare_abort_from_cv_exception(const cv::Exception &error, std::ostream &stream,
	                                     std::string_view context) -> CompareExit;
	auto compare_abort_from_exception(const std::exception &error, std::ostream &stream,
	                                  std::string_view context) -> CompareExit;
	auto compare_abort_from_unknown_exception(std::ostream &stream, std::string_view context)
	    -> CompareExit;

}  // namespace howdy::native
