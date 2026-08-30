#include "compare/logic.hpp"
#include "test_support.hpp"
#include "vision/frame_processing.hpp"
#include "vision/frame_validation.hpp"

#include <array>

namespace {

	using howdy::test::expect;
	using howdy::test::expect_near;

}  // namespace

auto main() -> int {
	bool ok = true;

	howdy::native::VideoConfig disabled_config{
	    .timeout              = 1,
	    .device_path          = "dummy",
	    .warn_no_device       = false,
	    .max_height           = 1.0F,
	    .frame_width          = 1,
	    .frame_height         = 1,
	    .clahe_enabled        = false,
	    .clahe_clip_limit     = 2.5F,
	    .clahe_tile_grid_size = 4,
	    .dark_threshold       = 50.0F,
	    .force_mjpeg          = false,
	    .exposure             = -1,
	    .device_fps           = 30,
	    .rotate               = 0,
	};
	auto disabled_clahe = howdy::native::make_clahe(disabled_config);
	ok &= expect(disabled_clahe.empty(), "disabled CLAHE does not allocate processor");

	cv::Mat    disabled_frame(8, 8, CV_8UC1, cv::Scalar(32));
	const auto disabled_before = disabled_frame.clone();
	howdy::native::apply_clahe_if_enabled(disabled_frame, disabled_config, disabled_clahe);
	ok &= expect(cv::countNonZero(disabled_frame != disabled_before) == 0,
	             "disabled CLAHE leaves frame unchanged");

	howdy::native::VideoConfig enabled_config{
	    .timeout              = 1,
	    .device_path          = "dummy",
	    .warn_no_device       = false,
	    .max_height           = 1.0F,
	    .frame_width          = 1,
	    .frame_height         = 1,
	    .clahe_enabled        = true,
	    .clahe_clip_limit     = 2.5F,
	    .clahe_tile_grid_size = 4,
	    .dark_threshold       = 50.0F,
	    .force_mjpeg          = false,
	    .exposure             = -1,
	    .device_fps           = 30,
	    .rotate               = 0,
	};
	auto enabled_clahe = howdy::native::make_clahe(enabled_config);
	ok &= expect(!enabled_clahe.empty(), "enabled CLAHE allocates processor");
	ok &= expect(enabled_clahe->getClipLimit() == enabled_config.clahe_clip_limit,
	             "CLAHE preserves configured clip limit");
	ok &= expect(enabled_clahe->getTilesGridSize() == cv::Size(enabled_config.clahe_tile_grid_size,
	                                                           enabled_config.clahe_tile_grid_size),
	             "CLAHE preserves configured tile grid size");

	cv::Mat enabled_frame(8, 8, CV_8UC1, cv::Scalar(32));
	howdy::native::apply_clahe_if_enabled(enabled_frame, enabled_config, enabled_clahe);
	ok &= expect(enabled_frame.rows == 8 && enabled_frame.cols == 8 &&
	                 enabled_frame.type() == CV_8UC1,
	             "enabled CLAHE preserves frame shape and type");

	cv::Ptr<cv::CLAHE> empty_enabled_clahe;
	cv::Mat            empty_enabled_frame(8, 8, CV_8UC1, cv::Scalar(48));
	howdy::native::apply_clahe_if_enabled(empty_enabled_frame, enabled_config, empty_enabled_clahe);
	ok &= expect(empty_enabled_frame.rows == 8 && empty_enabled_frame.cols == 8 &&
	                 empty_enabled_frame.type() == CV_8UC1,
	             "enabled CLAHE with empty processor preserves frame shape and type");

	ok &= expect(
	    howdy::native::validate_frame(cv::Mat(), howdy::native::FrameChannelPolicy::kCameraInput) ==
	        howdy::native::FrameValidationStatus::kEmpty,
	    "empty frame validation reports empty");

	const std::array<int, 3> three_d_sizes{2, 2, 2};
	const cv::Mat            three_d_frame(3, three_d_sizes.data(), CV_8UC1, cv::Scalar(0));
	ok &= expect(
	    howdy::native::validate_frame(three_d_frame, howdy::native::FrameChannelPolicy::kGray) ==
	        howdy::native::FrameValidationStatus::kUnsupportedDimensions,
	    "non-empty 3-D frame validation rejects unsupported dimensions");
	ok &= expect(howdy::native::validate_frame(cv::Mat(1, 1, CV_32FC1, cv::Scalar(0.0F)),
	                                           howdy::native::FrameChannelPolicy::kGray) ==
	                 howdy::native::FrameValidationStatus::kUnsupportedPixelType,
	             "grayscale validation rejects CV_32FC1");

	const cv::Mat max_boundary_frame(howdy::native::kMaxFrameDimension, 1, CV_8UC1, cv::Scalar(0));
	ok &= expect(howdy::native::validate_frame(max_boundary_frame,
	                                           howdy::native::FrameChannelPolicy::kGray) ==
	                 howdy::native::FrameValidationStatus::kValid,
	             "8192x1 grayscale frame is valid");
	ok &= expect(
	    howdy::native::validate_frame(cv::Mat(howdy::native::kMaxFrameDimension + 1, 1, CV_8UC1),
	                                  howdy::native::FrameChannelPolicy::kGray) ==
	        howdy::native::FrameValidationStatus::kOversizedDimensions,
	    "8193x1 grayscale frame is oversized");
	ok &= expect(
	    howdy::native::validate_frame(cv::Mat(1, howdy::native::kMaxFrameDimension + 1, CV_8UC1),
	                                  howdy::native::FrameChannelPolicy::kGray) ==
	        howdy::native::FrameValidationStatus::kOversizedDimensions,
	    "1x8193 grayscale frame is oversized");
	ok &= expect(howdy::native::validate_frame(cv::Mat(1, 1, CV_8UC1),
	                                           howdy::native::FrameChannelPolicy::kCameraInput) ==
	                 howdy::native::FrameValidationStatus::kValid,
	             "raw validation accepts 1 channel");
	ok &= expect(howdy::native::validate_frame(cv::Mat(1, 1, CV_8UC3),
	                                           howdy::native::FrameChannelPolicy::kCameraInput) ==
	                 howdy::native::FrameValidationStatus::kValid,
	             "raw validation accepts 3 channels");
	ok &= expect(howdy::native::validate_frame(cv::Mat(1, 1, CV_8UC4),
	                                           howdy::native::FrameChannelPolicy::kCameraInput) ==
	                 howdy::native::FrameValidationStatus::kValid,
	             "raw validation accepts 4 channels");
	ok &= expect(howdy::native::validate_frame(cv::Mat(1, 1, CV_8UC2),
	                                           howdy::native::FrameChannelPolicy::kCameraInput) ==
	                 howdy::native::FrameValidationStatus::kUnsupportedChannelCount,
	             "raw validation rejects unsupported channels");
	ok &= expect(howdy::native::validate_frame(cv::Mat(1, 1, CV_8UC3),
	                                           howdy::native::FrameChannelPolicy::kGray) ==
	                 howdy::native::FrameValidationStatus::kUnsupportedChannelCount,
	             "grayscale validation rejects 3 channels");
	ok &= expect(howdy::native::validate_frame(cv::Mat(1, 1, CV_8UC4),
	                                           howdy::native::FrameChannelPolicy::kGray) ==
	                 howdy::native::FrameValidationStatus::kUnsupportedChannelCount,
	             "grayscale validation rejects 4 channels");
	ok &= expect(howdy::native::validate_frame(cv::Mat(1, 1, CV_8UC3),
	                                           howdy::native::FrameChannelPolicy::kBgr) ==
	                 howdy::native::FrameValidationStatus::kValid,
	             "BGR validation accepts 3 channels");
	ok &= expect(howdy::native::validate_frame(cv::Mat(1, 1, CV_8UC1),
	                                           howdy::native::FrameChannelPolicy::kBgr) ==
	                 howdy::native::FrameValidationStatus::kUnsupportedChannelCount,
	             "BGR validation rejects 1 channel");
	ok &= expect(howdy::native::validate_frame(cv::Mat(1, 1, CV_8UC4),
	                                           howdy::native::FrameChannelPolicy::kBgr) ==
	                 howdy::native::FrameValidationStatus::kUnsupportedChannelCount,
	             "BGR validation rejects 4 channels");

	const auto empty_brightness = howdy::native::measure_brightness(cv::Mat());
	ok &= expect(empty_brightness.hist_total == 0.0 && empty_brightness.darkness == 100.0F,
	             "empty frame reports zero histogram and full darkness");

	const cv::Mat black_frame(4, 4, CV_8UC1, cv::Scalar(0));
	const auto    black_brightness = howdy::native::measure_brightness(black_frame);
	ok &= expect(black_brightness.hist_total == 16.0 && black_brightness.darkness == 100.0F,
	             "non-empty black frame reports full darkness");
	ok &= expect(black_brightness.hist_total > 0.0,
	             "non-empty black frame keeps non-zero histogram total");
	int valid_frames = 0;
	int dark_tries   = 0;
	if (black_brightness.hist_total == 0.0 || black_brightness.darkness >= 100.0F) {
	} else {
		switch (howdy::native::classify_brightness(black_brightness.hist_total,
		                                           black_brightness.darkness, 30.0F)) {
			case howdy::native::BrightnessDecision::kBlackFrame:
				break;
			case howdy::native::BrightnessDecision::kTooDark:
				valid_frames++;
				dark_tries++;
				break;
			case howdy::native::BrightnessDecision::kProcessFrame:
				valid_frames++;
				break;
		}
	}
	ok &= expect(valid_frames == 0 && dark_tries == 0,
	             "non-empty black frame is skipped before enrollment counters");
	ok &= expect(howdy::native::classify_brightness(black_brightness.hist_total,
	                                                black_brightness.darkness, 30.0F) ==
	                 howdy::native::BrightnessDecision::kBlackFrame,
	             "non-empty black frame is skipped");
	ok &= expect(black_brightness.bins_percent[0] == 100.0F,
	             "black frame fills darkest histogram bin");

	const cv::Mat normal_frame = cv::Mat_<unsigned char>(
	    {4, 4}, {0, 0, 0, 0, 32, 64, 64, 96, 128, 128, 160, 192, 192, 224, 224, 224});
	const auto normal_brightness = howdy::native::measure_brightness(normal_frame);
	ok &= expect_near(normal_brightness.darkness, 25.0, 0.0001,
	                  "normal frame darkness uses darkest histogram bin");
	constexpr std::array<double, 8> expected_bins{25.0, 6.25, 12.5, 6.25, 12.5, 6.25, 12.5, 18.75};
	for (std::size_t index = 0; index < expected_bins.size(); ++index) {
		ok &= expect_near(normal_brightness.bins_percent[index], expected_bins[index], 0.0001,
		                  "normal frame histogram bin percentage is stable");
	}
	ok &= expect(howdy::native::classify_brightness(normal_brightness.hist_total,
	                                                normal_brightness.darkness, 30.0F) ==
	                 howdy::native::BrightnessDecision::kProcessFrame,
	             "normal frame below threshold remains processable");
	ok &= expect(howdy::native::classify_brightness(normal_brightness.hist_total,
	                                                normal_brightness.darkness, 25.0F) ==
	                 howdy::native::BrightnessDecision::kProcessFrame,
	             "brightness at threshold remains processable");
	ok &= expect(howdy::native::classify_brightness(normal_brightness.hist_total,
	                                                normal_brightness.darkness, 24.9F) ==
	                 howdy::native::BrightnessDecision::kTooDark,
	             "brightness above threshold is too dark");

	return ok ? 0 : 1;
}
