#include "vision/frame_processing.hpp"

#include <algorithm>
#include <vector>

namespace howdy::native {

	auto make_clahe(const VideoConfig &config) -> cv::Ptr<cv::CLAHE> {
		if (!config.clahe_enabled) {
			return {};
		}
		return cv::createCLAHE(config.clahe_clip_limit,
		                       cv::Size(config.clahe_tile_grid_size, config.clahe_tile_grid_size));
	}

	void apply_clahe_if_enabled(cv::Mat &gray, const VideoConfig &config,
	                            cv::Ptr<cv::CLAHE> &clahe) {
		if (config.clahe_enabled && !clahe.empty()) {
			clahe->apply(gray, gray);
		}
	}

	auto measure_brightness(const cv::Mat &gray) -> BrightnessStats {
		BrightnessStats stats;
		if (gray.empty()) {
			return stats;
		}

		cv::Mat                         hist;
		static const std::vector<int>   hist_size{8};
		static const std::vector<float> hist_range{0.0F, 256.0F};
		static const std::vector<int>   channels{0};
		const std::vector<cv::Mat>      images{gray};
		cv::calcHist(images, channels, cv::Mat(), hist, hist_size, hist_range);

		stats.hist_total = cv::sum(hist)[0];
		if (stats.hist_total != 0.0) {
			stats.darkness = static_cast<float>(hist.at<float>(0) / stats.hist_total * 100.0);
		}

		const auto bins_denominator = std::max(static_cast<float>(stats.hist_total), 1.0F);
		for (std::size_t index = 0; index < stats.bins_percent.size(); ++index) {
			stats.bins_percent[index] =
			    hist.at<float>(static_cast<int>(index)) / bins_denominator * 100.0F;
		}
		return stats;
	}

}  // namespace howdy::native
