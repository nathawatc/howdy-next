#include "vision/frame_processing.hpp"

#include "config/runtime_config.hpp"

#include <algorithm>
#include <vector>

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

	auto MeasureBrightness(const cv::Mat &gray) -> BrightnessStats {
		BrightnessStats stats;
		if (gray.empty()) {
			return stats;
		}

		cv::Mat                         hist;
		static const std::vector<int>   kHistSize{8};
		static const std::vector<float> kHistRange{0.0F, 256.0F};
		static const std::vector<int>   kChannels{0};
		const std::vector<cv::Mat>      images{gray};
		cv::calcHist(images, kChannels, cv::Mat(), hist, kHistSize, kHistRange);

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

}  // namespace howdy::native
