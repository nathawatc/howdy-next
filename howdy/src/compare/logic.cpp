#include "compare/logic.hpp"

#include "vision/frame_processing.hpp"

#include <algorithm>
#include <cmath>
#include <ostream>

namespace howdy::native {

	auto UpdateBestScore(float current, float score, FaceMetric metric) -> float {
		const auto *policy = GetFaceMetricPolicy(metric);
		if (policy == nullptr) {
			return current;
		}
		if (std::isnan(current)) {
			return score;
		}
		return policy->higher_score_is_better ? std::max(current, score) : std::min(current, score);
	}

	auto ClassifyBrightness(double hist_total, float darkness, float dark_threshold)
	    -> BrightnessDecision {
		if (hist_total == 0.0 || darkness == kBrightnessPercentScale) {
			return BrightnessDecision::kBlackFrame;
		}
		if (darkness > dark_threshold) {
			return BrightnessDecision::kTooDark;
		}
		return BrightnessDecision::kProcessFrame;
	}

	auto TimeoutExit(int dark_tries, int valid_frames) -> CompareExit {
		if (dark_tries > 0 && valid_frames == dark_tries) {
			return CompareExit::kTooDark;
		}
		return CompareExit::kTimeoutReached;
	}

	auto CompareResizeScale(FrameGeometry frame, float max_height) -> double {
		const int scaling_axis = frame.rotation == 2 ? frame.width : frame.height;
		if (scaling_axis <= 0 || max_height <= 0.0F) {
			return 1.0;
		}

		const double scale = static_cast<double>(max_height) / static_cast<double>(scaling_axis);
		if (!std::isfinite(scale) || scale >= 1.0) {
			return 1.0;
		}
		return scale;
	}

	auto CompareAbortFromCvException(const cv::Exception &error, std::ostream &stream,
	                                 std::string_view context) -> CompareExit {
		stream << "OpenCV exception during " << context << ": " << error.what() << "\n";
		return CompareExit::kAbort;
	}

	auto CompareAbortFromException(const std::exception &error, std::ostream &stream,
	                               std::string_view context) -> CompareExit {
		stream << "Unhandled exception during " << context << ": " << error.what() << "\n";
		return CompareExit::kAbort;
	}

	auto CompareAbortFromUnknownException(std::ostream &stream, std::string_view context)
	    -> CompareExit {
		stream << "Unknown exception during " << context << "\n";
		return CompareExit::kAbort;
	}

}  // namespace howdy::native
