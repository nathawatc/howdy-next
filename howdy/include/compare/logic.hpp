#pragma once

#include "protocol/compare_exit.hpp"

#include <exception>
#include <iosfwd>
#include <string_view>

#include <opencv2/core.hpp>

namespace howdy::native {
	inline constexpr auto kAllFramesTooDarkMessage =
	    "All frames were too dark; check dark_threshold";
	inline constexpr auto kAverageDarknessLabel = "Average darkness: ";
	inline constexpr auto kThresholdLabel       = ", Threshold: ";

	[[nodiscard]] auto TimeoutExit(int dark_tries, int valid_frames) -> CompareExit;
	[[nodiscard]] auto CompareAbortFromCvException(const cv::Exception &error, std::ostream &stream,
	                                               std::string_view context) -> CompareExit;
	[[nodiscard]] auto CompareAbortFromException(const std::exception &error, std::ostream &stream,
	                                             std::string_view context) -> CompareExit;
	[[nodiscard]] auto CompareAbortFromUnknownException(std::ostream    &stream,
	                                                    std::string_view context) -> CompareExit;

}  // namespace howdy::native
