#include "compare/compare_engine_test_support.hpp"
#include "compare/engine.hpp"
#include "test_support.hpp"
#include "vision/frame_processing.hpp"

#include <string>

#include <opencv2/core.hpp>
#include <opencv2/core/hal/interface.h>
#include <opencv2/core/mat.hpp>
#include <opencv2/core/types.hpp>
#include <opencv2/imgproc.hpp>

namespace {

	using howdy::test::Expect;
	using howdy::test::compare_engine::ExpectNear;
	using howdy::test::compare_engine::MakeVideoConfig;

	auto ExpectMatrixEqual(const cv::Mat &actual, const cv::Mat &expected,
	                       const std::string &message) -> bool {
		if (actual.size() != expected.size() || actual.type() != expected.type()) {
			return Expect(false, message);
		}
		return Expect(cv::countNonZero(actual != expected) == 0, message);
	}

	auto MakeQuarterDarkFrame() -> cv::Mat {
		cv::Mat frame(4, 4, CV_8UC1, cv::Scalar(128));
		frame.row(0).setTo(cv::Scalar(0));
		return frame;
	}

	auto MakePreprocessingFrame() -> cv::Mat {
		cv::Mat frame(12, 16, CV_8UC1);
		for (int row = 0; row < frame.rows; ++row) {
			for (int column = 0; column < frame.cols; ++column) {
				frame.at<unsigned char>(row, column) = static_cast<unsigned char>(
				    ((row * 31) + (column * 17) + ((row * column) * 7)) % 256);
			}
		}
		return frame;
	}

}  // namespace

auto RunCompareEngineFrameTests() -> bool {
	using howdy::native::CompareFrameStatus;

	bool ok = true;

	{
		howdy::native::CompareEngine engine(MakeVideoConfig());
		const auto                   result = engine.ProcessGrayFrame(cv::Mat(), 1);
		ok &= Expect(result.status == CompareFrameStatus::kInvalidInput,
		             "empty frame is invalid input");
		ok &= Expect(result.error_message == "Camera grayscale frame is empty",
		             "empty frame error is stable");
	}

	{
		howdy::native::CompareEngine engine(MakeVideoConfig(25.0F, 100.0F, 0, true));
		const auto                   result =
		    engine.ProcessGrayFrame(cv::Mat(2, 2, CV_8UC3, cv::Scalar(32, 64, 96)), 1);
		ok &= Expect(result.status == CompareFrameStatus::kInvalidInput,
		             "BGR frame is invalid grayscale input");
		ok &= Expect(result.error_message ==
		                 "Camera grayscale frame has unsupported channel count: 3",
		             "BGR frame channel error is stable");
	}

	{
		howdy::native::CompareEngine engine(MakeVideoConfig());
		const auto result = engine.ProcessGrayFrame(cv::Mat(2, 2, CV_32FC1, cv::Scalar(64.0F)), 1);
		ok &= Expect(result.status == CompareFrameStatus::kInvalidInput,
		             "floating-point frame is invalid input");
		ok &= Expect(result.error_message == "Camera grayscale frame has unsupported pixel type: " +
		                                         std::to_string(CV_32FC1),
		             "floating-point frame pixel error is stable");
	}

	{
		howdy::native::CompareEngine engine(MakeVideoConfig());
		const auto result = engine.ProcessGrayFrame(cv::Mat(4, 4, CV_8UC1, cv::Scalar(0)), 1);
		ok &= Expect(result.status == CompareFrameStatus::kBlackFrame,
		             "black frame is classified before inference");
		ok &= Expect(result.brightness.hist_total > 0.0,
		             "black frame retains non-zero histogram total");
		ok &= ExpectNear(result.brightness.darkness, 100.0, 0.0001,
		                 "black frame retains full darkness");
		ok &= Expect(result.working_frame.empty(), "black frame returns no working frame");
	}

	{
		const auto                   config = MakeVideoConfig(25.0F, 8.0F);
		howdy::native::CompareEngine engine(config);
		const auto result = engine.ProcessGrayFrame(cv::Mat(16, 16, CV_8UC1, cv::Scalar(0)), 1);
		ok &= Expect(result.status == CompareFrameStatus::kBlackFrame,
		             "resized black frame remains black with CLAHE disabled");
		ok &= ExpectNear(result.brightness.hist_total, 64.0, 0.0001,
		                 "black-frame histogram covers resized image");
		ok &= Expect(result.working_frame.empty(), "resized black frame returns no working frame");
	}

	{
		howdy::native::CompareEngine engine(MakeVideoConfig(24.0F));
		const auto                   result = engine.ProcessGrayFrame(MakeQuarterDarkFrame(), 1);
		ok &= Expect(result.status == CompareFrameStatus::kTooDark,
		             "frame above dark threshold is too dark");
		ok &= ExpectNear(result.brightness.hist_total, 16.0, 0.0001,
		                 "too-dark frame retains histogram total");
		ok &=
		    ExpectNear(result.brightness.darkness, 25.0, 0.0001, "too-dark frame retains darkness");
		ok &= Expect(result.working_frame.empty(), "too-dark frame returns no working frame");
	}

	{
		howdy::native::CompareEngine engine(MakeVideoConfig(25.0F));
		const auto                   result = engine.ProcessGrayFrame(MakeQuarterDarkFrame(), 1);
		ok &= Expect(result.status == CompareFrameStatus::kReady,
		             "frame at dark threshold is processable");
		ok &= ExpectNear(result.brightness.darkness, 25.0, 0.0001,
		                 "processable frame retains darkness");
		ok &= Expect(result.working_frame.rows == 4 && result.working_frame.cols == 4 &&
		                 result.working_frame.type() == CV_8UC1,
		             "processable frame preserves dimensions and type");
	}

	{
		cv::Mat const                source(4, 6, CV_8UC1, cv::Scalar(128));
		howdy::native::CompareEngine engine(MakeVideoConfig(25.0F, 2.0F));
		const auto                   result = engine.ProcessGrayFrame(source, 1);
		ok &= Expect(result.status == CompareFrameStatus::kReady, "downscaled frame is ready");
		ok &= Expect(result.working_frame.rows == 2 && result.working_frame.cols == 3,
		             "downscale uses compare resize scale");
	}

	{
		const cv::Mat                source(8192, 1, CV_8UC1, cv::Scalar(255));
		howdy::native::CompareEngine engine(MakeVideoConfig(25.0F, 32.0F));
		const auto                   result = engine.ProcessGrayFrame(source, 1);
		ok &= Expect(result.status == CompareFrameStatus::kReady,
		             "narrow valid frame survives compare downscale");
		ok &= Expect(result.working_frame.rows == 32 && result.working_frame.cols == 1,
		             "compare resize clamps rounded dimensions to one pixel");
	}

	{
		const cv::Mat source(8, 8, CV_8UC1, cv::Scalar(64));
		source.rowRange(0, 4).setTo(cv::Scalar(0));
		howdy::native::CompareEngine engine(MakeVideoConfig(40.0F, 4.0F));
		const auto                   result = engine.ProcessGrayFrame(source, 1);
		ok &= Expect(result.status == CompareFrameStatus::kTooDark,
		             "dark frame remains too dark after resize");
		ok &= ExpectNear(result.brightness.hist_total, 16.0, 0.0001,
		                 "too-dark histogram covers resized image");
		ok &= ExpectNear(result.brightness.darkness, 50.0, 0.0001,
		                 "too-dark classification measures resized pixels");
		ok &=
		    Expect(result.working_frame.empty(), "resized too-dark frame returns no working frame");
	}

	{
		const auto                   source = MakePreprocessingFrame();
		const auto                   config = MakeVideoConfig(100.0F, 6.0F, 1, true);
		howdy::native::CompareEngine engine(config);
		const auto                   result = engine.ProcessGrayFrame(source.clone(), 1);

		cv::Mat expected;
		cv::resize(source, expected, cv::Size(), 0.5, 0.5, cv::INTER_AREA);
		auto clahe = howdy::native::MakeClahe(config);
		howdy::native::ApplyClaheIfEnabled(expected, config, clahe);
		const auto expected_brightness = howdy::native::MeasureBrightness(expected);
		expected                       = howdy::native::RotateFrame(expected, config, 1);
		ok &= Expect(result.status == CompareFrameStatus::kReady,
		             "resize-before-CLAHE frame is processable");
		ok &= ExpectMatrixEqual(result.working_frame, expected,
		                        "compare resizes before CLAHE and preserves rotation order");
		ok &= ExpectNear(result.brightness.hist_total, 48.0, 0.0001,
		                 "brightness histogram covers resized frame after CLAHE");
		ok &= ExpectNear(result.brightness.darkness, expected_brightness.darkness, 0.0001,
		                 "brightness classification measures resized CLAHE output");

		auto threshold_config = MakeVideoConfig(expected_brightness.darkness, 6.0F, 0, true);
		howdy::native::CompareEngine threshold_engine(threshold_config);
		const auto threshold_result = threshold_engine.ProcessGrayFrame(source.clone(), 1);
		ok &= Expect(threshold_result.status == CompareFrameStatus::kReady,
		             "resized-frame brightness at dark threshold remains processable");
		ok &= ExpectNear(threshold_result.brightness.darkness, expected_brightness.darkness, 0.0001,
		                 "dark threshold uses resized-frame CLAHE statistics");
	}

	{
		cv::Mat const                source(2, 3, CV_8UC1, cv::Scalar(128));
		howdy::native::CompareEngine engine(MakeVideoConfig(25.0F, 8.0F));
		const auto                   result = engine.ProcessGrayFrame(source, 1);
		ok &= Expect(result.status == CompareFrameStatus::kReady, "small frame is ready");
		ok &= Expect(result.working_frame.rows == 2 && result.working_frame.cols == 3,
		             "small frame is not upscaled");
	}

	const cv::Mat source = cv::Mat_<uchar>({2, 3}, {32, 64, 96, 128, 160, 192});

	{
		howdy::native::CompareEngine engine(MakeVideoConfig(100.0F, 100.0F, 0));
		const auto                   result = engine.ProcessGrayFrame(source.clone(), 1);
		ok &= Expect(result.status == CompareFrameStatus::kReady, "rotate 0 frame is ready");
		ok &= ExpectMatrixEqual(result.working_frame, source, "rotate 0 frame remains unchanged");
	}

	{
		cv::Mat expected;
		cv::rotate(source, expected, cv::ROTATE_90_COUNTERCLOCKWISE);
		howdy::native::CompareEngine engine(MakeVideoConfig(100.0F, 100.0F, 1));
		const auto                   result = engine.ProcessGrayFrame(source.clone(), 1);
		ok &= Expect(result.status == CompareFrameStatus::kReady, "rotate 1 frame 1 is ready");
		ok &= ExpectMatrixEqual(result.working_frame, expected,
		                        "rotate 1 frame 1 rotates counterclockwise");
	}

	{
		cv::Mat expected;
		cv::rotate(source, expected, cv::ROTATE_90_CLOCKWISE);
		howdy::native::CompareEngine engine(MakeVideoConfig(100.0F, 100.0F, 1));
		const auto                   result = engine.ProcessGrayFrame(source.clone(), 2);
		ok &= Expect(result.status == CompareFrameStatus::kReady, "rotate 1 frame 2 is ready");
		ok &=
		    ExpectMatrixEqual(result.working_frame, expected, "rotate 1 frame 2 rotates clockwise");
	}

	{
		howdy::native::CompareEngine engine(MakeVideoConfig(100.0F, 100.0F, 1));
		const auto                   result = engine.ProcessGrayFrame(source.clone(), 3);
		ok &= Expect(result.status == CompareFrameStatus::kReady, "rotate 1 frame 3 is ready");
		ok &= ExpectMatrixEqual(result.working_frame, source, "rotate 1 frame 3 remains unchanged");
	}

	{
		cv::Mat expected;
		cv::rotate(source, expected, cv::ROTATE_90_CLOCKWISE);
		howdy::native::CompareEngine engine(MakeVideoConfig(100.0F, 100.0F, 2));
		const auto                   result = engine.ProcessGrayFrame(source.clone(), 1);
		ok &= Expect(result.status == CompareFrameStatus::kReady, "rotate 2 frame 1 is ready");
		ok &=
		    ExpectMatrixEqual(result.working_frame, expected, "rotate 2 frame 1 rotates clockwise");
	}

	{
		cv::Mat expected;
		cv::rotate(source, expected, cv::ROTATE_90_COUNTERCLOCKWISE);
		howdy::native::CompareEngine engine(MakeVideoConfig(100.0F, 100.0F, 2));
		const auto                   result = engine.ProcessGrayFrame(source.clone(), 2);
		ok &= Expect(result.status == CompareFrameStatus::kReady, "rotate 2 frame 2 is ready");
		ok &= ExpectMatrixEqual(result.working_frame, expected,
		                        "rotate 2 frame 2 rotates counterclockwise");
	}

	{
		cv::Mat expected;
		cv::rotate(source, expected, cv::ROTATE_180);
		howdy::native::CompareEngine engine(MakeVideoConfig(100.0F, 100.0F, 3));
		for (int frame_number = 1; frame_number <= 3; ++frame_number) {
			const auto result = engine.ProcessGrayFrame(source.clone(), frame_number);
			ok &= Expect(result.status == CompareFrameStatus::kReady,
			             "rotate 3 compare frame is ready");
			ok &= ExpectMatrixEqual(result.working_frame, expected,
			                        "rotate 3 compare frame is fixed 180 degrees");
		}
	}

	{
		const cv::Mat fixture(16, 16, CV_8UC1, cv::Scalar(16));

		howdy::native::CompareEngine disabled_engine(MakeVideoConfig(50.0F, 100.0F, 0, false));
		const auto disabled_result = disabled_engine.ProcessGrayFrame(fixture.clone(), 1);
		ok &= Expect(disabled_result.status == CompareFrameStatus::kBlackFrame,
		             "dark fixture without CLAHE is black");
		ok &= ExpectNear(disabled_result.brightness.hist_total, 256.0, 0.0001,
		                 "dark fixture without CLAHE retains histogram total");
		ok &= ExpectNear(disabled_result.brightness.darkness, 100.0, 0.0001,
		                 "dark fixture without CLAHE reports full darkness");

		howdy::native::CompareEngine enabled_engine(MakeVideoConfig(50.0F, 100.0F, 0, true));
		const auto enabled_result = enabled_engine.ProcessGrayFrame(fixture.clone(), 1);
		ok &= Expect(enabled_result.status == CompareFrameStatus::kReady,
		             "CLAHE lifts fixture before brightness classification");
		ok &= ExpectNear(enabled_result.brightness.hist_total, 256.0, 0.0001,
		                 "CLAHE fixture retains histogram total");
		ok &= ExpectNear(enabled_result.brightness.darkness, 0.0, 0.0001,
		                 "CLAHE fixture reports no darkest-bin pixels");
	}

	return ok;
}
