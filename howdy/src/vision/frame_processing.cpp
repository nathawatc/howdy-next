#include "vision/frame_processing.hpp"

#include "config/runtime_config.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include <opencv2/core.hpp>
#include <opencv2/core/cvstd_wrapper.hpp>
#include <opencv2/core/mat.hpp>
#include <opencv2/imgproc.hpp>

namespace howdy::native {

	auto MakeClahe(const VideoConfig &config) -> cv::Ptr<cv::CLAHE> {
		if (!config.clahe_enabled) {
			return {};
		}
		return cv::createCLAHE(config.clahe_clip_limit,
		                       cv::Size(config.clahe_tile_grid_size, config.clahe_tile_grid_size));
	}

	void ApplyClaheIfEnabled(cv::Mat &gray, const VideoConfig &config, cv::Ptr<cv::CLAHE> &clahe) {
		if (config.clahe_enabled && !clahe.empty()) {
			clahe->apply(gray, gray);
		}
	}

	auto RotateFrame(const cv::Mat &frame, const VideoConfig &config, int frame_number) -> cv::Mat {
		if (config.rotate == 1) {
			if (frame_number % 3 == 1) {
				cv::Mat rotated;
				cv::rotate(frame, rotated, cv::ROTATE_90_COUNTERCLOCKWISE);
				return rotated;
			}
			if (frame_number % 3 == 2) {
				cv::Mat rotated;
				cv::rotate(frame, rotated, cv::ROTATE_90_CLOCKWISE);
				return rotated;
			}
		} else if (config.rotate == 2) {
			cv::Mat rotated;
			cv::rotate(frame, rotated,
			           frame_number % 2 == 0 ? cv::ROTATE_90_COUNTERCLOCKWISE
			                                 : cv::ROTATE_90_CLOCKWISE);
			return rotated;
		} else if (config.rotate == 3) {
			cv::Mat rotated;
			cv::rotate(frame, rotated, cv::ROTATE_180);
			return rotated;
		}

		return frame;
	}

	auto MeasureBrightness(const cv::Mat &gray) -> BrightnessStats {
		BrightnessStats stats;
		if (gray.empty()) {
			return stats;
		}

		cv::Mat                        hist;
		constexpr std::array<int, 1>   channels{0};
		constexpr std::array<int, 1>   hist_size{8};
		constexpr std::array<float, 2> range{0.0F, 256.0F};
		const float                   *ranges = range.data();
		cv::calcHist(&gray, 1, channels.data(), cv::Mat(), hist, 1, hist_size.data(), &ranges);

		stats.hist_total = cv::sum(hist).val[0];
		if (stats.hist_total != 0.0) {
			stats.darkness = static_cast<float>(hist.at<float>(0) / stats.hist_total *
			                                    static_cast<double>(kBrightnessPercentScale));
		}

		const auto bins_denominator = std::max(static_cast<float>(stats.hist_total), 1.0F);
		int        hist_index       = 0;
		for (auto &bin_percent : stats.bins_percent) {
			bin_percent = hist.at<float>(hist_index) / bins_denominator * kBrightnessPercentScale;
			++hist_index;
		}
		return stats;
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

	auto FrameResizeScale(FrameGeometry frame, float max_height) -> double {
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

}  // namespace howdy::native
