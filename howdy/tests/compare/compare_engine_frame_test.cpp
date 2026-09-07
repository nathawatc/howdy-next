#include "compare/compare_engine_test_support.hpp"
#include "compare/engine.hpp"

#include <string>

#include <opencv2/imgproc.hpp>

namespace {

	using howdy::test::expect;
	using howdy::test::compare_engine::expect_near;
	using howdy::test::compare_engine::MakeVideoConfig;

	auto ExpectMatrixEqual(const cv::Mat &actual, const cv::Mat &expected,
	                       const std::string &message) -> bool {
		if (actual.size() != expected.size() || actual.type() != expected.type()) {
			return expect(false, message);
		}
		return expect(cv::countNonZero(actual != expected) == 0, message);
	}

	auto MakeQuarterDarkFrame() -> cv::Mat {
		cv::Mat frame(4, 4, CV_8UC1, cv::Scalar(128));
		frame.row(0).setTo(cv::Scalar(0));
		return frame;
	}

}  // namespace

auto RunCompareEngineFrameTests() -> bool {
	using howdy::native::CompareFrameStatus;

	bool ok = true;

	{
		howdy::native::CompareEngine engine(MakeVideoConfig());
		const auto                   result = engine.ProcessGrayFrame(cv::Mat(), 1);
		ok &= expect(result.status == CompareFrameStatus::kInvalidInput,
		             "empty frame is invalid input");
		ok &= expect(result.error_message == "Camera grayscale frame is empty",
		             "empty frame error is stable");
	}

	{
		howdy::native::CompareEngine engine(MakeVideoConfig());
		const auto                   result =
		    engine.ProcessGrayFrame(cv::Mat(2, 2, CV_8UC3, cv::Scalar(32, 64, 96)), 1);
		ok &= expect(result.status == CompareFrameStatus::kInvalidInput,
		             "BGR frame is invalid grayscale input");
		ok &= expect(result.error_message ==
		                 "Camera grayscale frame has unsupported channel count: 3",
		             "BGR frame channel error is stable");
	}

	{
		howdy::native::CompareEngine engine(MakeVideoConfig());
		const auto result = engine.ProcessGrayFrame(cv::Mat(2, 2, CV_32FC1, cv::Scalar(64.0F)), 1);
		ok &= expect(result.status == CompareFrameStatus::kInvalidInput,
		             "floating-point frame is invalid input");
		ok &= expect(result.error_message == "Camera grayscale frame has unsupported pixel type: " +
		                                         std::to_string(CV_32FC1),
		             "floating-point frame pixel error is stable");
	}

	{
		howdy::native::CompareEngine engine(MakeVideoConfig());
		const auto result = engine.ProcessGrayFrame(cv::Mat(4, 4, CV_8UC1, cv::Scalar(0)), 1);
		ok &= expect(result.status == CompareFrameStatus::kBlackFrame,
		             "black frame is classified before preprocessing");
		ok &= expect(result.brightness.hist_total > 0.0,
		             "black frame retains non-zero histogram total");
		ok &= expect_near(result.brightness.darkness, 100.0, 0.0001,
		                  "black frame retains full darkness");
		ok &= expect(result.working_frame.empty(), "black frame returns no working frame");
	}

	{
		howdy::native::CompareEngine engine(MakeVideoConfig(24.0F));
		const auto                   result = engine.ProcessGrayFrame(MakeQuarterDarkFrame(), 1);
		ok &= expect(result.status == CompareFrameStatus::kTooDark,
		             "frame above dark threshold is too dark");
		ok &= expect_near(result.brightness.hist_total, 16.0, 0.0001,
		                  "too-dark frame retains histogram total");
		ok &= expect_near(result.brightness.darkness, 25.0, 0.0001,
		                  "too-dark frame retains darkness");
		ok &= expect(result.working_frame.empty(), "too-dark frame returns no working frame");
	}

	{
		howdy::native::CompareEngine engine(MakeVideoConfig(25.0F));
		const auto                   result = engine.ProcessGrayFrame(MakeQuarterDarkFrame(), 1);
		ok &= expect(result.status == CompareFrameStatus::kReady,
		             "frame at dark threshold is processable");
		ok &= expect_near(result.brightness.darkness, 25.0, 0.0001,
		                  "processable frame retains darkness");
		ok &= expect(result.working_frame.rows == 4 && result.working_frame.cols == 4 &&
		                 result.working_frame.type() == CV_8UC1,
		             "processable frame preserves dimensions and type");
	}

	{
		cv::Mat                      source(4, 6, CV_8UC1, cv::Scalar(128));
		howdy::native::CompareEngine engine(MakeVideoConfig(25.0F, 2.0F));
		const auto                   result = engine.ProcessGrayFrame(source, 1);
		ok &= expect(result.status == CompareFrameStatus::kReady, "downscaled frame is ready");
		ok &= expect(result.working_frame.rows == 2 && result.working_frame.cols == 3,
		             "downscale uses compare resize scale");
	}

	{
		cv::Mat                      source(2, 3, CV_8UC1, cv::Scalar(128));
		howdy::native::CompareEngine engine(MakeVideoConfig(25.0F, 8.0F));
		const auto                   result = engine.ProcessGrayFrame(source, 1);
		ok &= expect(result.status == CompareFrameStatus::kReady, "small frame is ready");
		ok &= expect(result.working_frame.rows == 2 && result.working_frame.cols == 3,
		             "small frame is not upscaled");
	}

	const cv::Mat source = cv::Mat_<uchar>({2, 3}, {32, 64, 96, 128, 160, 192});

	{
		cv::Mat expected;
		cv::rotate(source, expected, cv::ROTATE_90_COUNTERCLOCKWISE);
		howdy::native::CompareEngine engine(MakeVideoConfig(100.0F, 100.0F, 1));
		const auto                   result = engine.ProcessGrayFrame(source.clone(), 1);
		ok &= expect(result.status == CompareFrameStatus::kReady, "rotate 1 frame 1 is ready");
		ok &= ExpectMatrixEqual(result.working_frame, expected,
		                        "rotate 1 frame 1 rotates counterclockwise");
	}

	{
		cv::Mat expected;
		cv::rotate(source, expected, cv::ROTATE_90_CLOCKWISE);
		howdy::native::CompareEngine engine(MakeVideoConfig(100.0F, 100.0F, 1));
		const auto                   result = engine.ProcessGrayFrame(source.clone(), 2);
		ok &= expect(result.status == CompareFrameStatus::kReady, "rotate 1 frame 2 is ready");
		ok &=
		    ExpectMatrixEqual(result.working_frame, expected, "rotate 1 frame 2 rotates clockwise");
	}

	{
		howdy::native::CompareEngine engine(MakeVideoConfig(100.0F, 100.0F, 1));
		const auto                   result = engine.ProcessGrayFrame(source.clone(), 3);
		ok &= expect(result.status == CompareFrameStatus::kReady, "rotate 1 frame 3 is ready");
		ok &= ExpectMatrixEqual(result.working_frame, source, "rotate 1 frame 3 remains unchanged");
	}

	{
		cv::Mat expected;
		cv::rotate(source, expected, cv::ROTATE_90_CLOCKWISE);
		howdy::native::CompareEngine engine(MakeVideoConfig(100.0F, 100.0F, 2));
		const auto                   result = engine.ProcessGrayFrame(source.clone(), 1);
		ok &= expect(result.status == CompareFrameStatus::kReady, "rotate 2 frame 1 is ready");
		ok &=
		    ExpectMatrixEqual(result.working_frame, expected, "rotate 2 frame 1 rotates clockwise");
	}

	{
		cv::Mat expected;
		cv::rotate(source, expected, cv::ROTATE_90_COUNTERCLOCKWISE);
		howdy::native::CompareEngine engine(MakeVideoConfig(100.0F, 100.0F, 2));
		const auto                   result = engine.ProcessGrayFrame(source.clone(), 2);
		ok &= expect(result.status == CompareFrameStatus::kReady, "rotate 2 frame 2 is ready");
		ok &= ExpectMatrixEqual(result.working_frame, expected,
		                        "rotate 2 frame 2 rotates counterclockwise");
	}

	{
		const cv::Mat fixture(16, 16, CV_8UC1, cv::Scalar(16));

		howdy::native::CompareEngine disabled_engine(MakeVideoConfig(50.0F, 100.0F, 0, false));
		const auto disabled_result = disabled_engine.ProcessGrayFrame(fixture.clone(), 1);
		ok &= expect(disabled_result.status == CompareFrameStatus::kBlackFrame,
		             "dark fixture without CLAHE is black");
		ok &= expect_near(disabled_result.brightness.hist_total, 256.0, 0.0001,
		                  "dark fixture without CLAHE retains histogram total");
		ok &= expect_near(disabled_result.brightness.darkness, 100.0, 0.0001,
		                  "dark fixture without CLAHE reports full darkness");

		howdy::native::CompareEngine enabled_engine(MakeVideoConfig(50.0F, 100.0F, 0, true));
		const auto enabled_result = enabled_engine.ProcessGrayFrame(fixture.clone(), 1);
		ok &= expect(enabled_result.status == CompareFrameStatus::kReady,
		             "CLAHE lifts fixture before brightness classification");
		ok &= expect_near(enabled_result.brightness.hist_total, 256.0, 0.0001,
		                  "CLAHE fixture retains histogram total");
		ok &= expect_near(enabled_result.brightness.darkness, 0.0, 0.0001,
		                  "CLAHE fixture reports no darkest-bin pixels");
	}

	return ok;
}
