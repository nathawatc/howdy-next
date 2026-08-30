#include "test_support.hpp"
#include "vision/frame_validation.hpp"
#include "vision/video_capture.hpp"

#include <array>
#include <memory>
#include <string>
#include <utility>

namespace howdy::native {

	class VideoCaptureTestAccess {
	public:
		class ReadContext final : public VideoCapture::FrameReader {
		public:
			explicit ReadContext(cv::Mat source_frame)
			    : frame(std::move(source_frame)) {}

			auto read(cv::Mat &output) -> bool override {
				calls++;
				output = frame.clone();
				return true;
			}

			cv::Mat frame;
			int     calls = 0;
		};

		static auto create(CaptureSettings settings, std::shared_ptr<ReadContext> frame_reader)
		    -> VideoCapture {
			return {std::move(settings), std::move(frame_reader)};
		}
	};

}  // namespace howdy::native

namespace {

	using howdy::test::expect;

	using ReadContext = howdy::native::VideoCaptureTestAccess::ReadContext;

	auto gray_sentinel() -> cv::Mat {
		return {1, 1, CV_8UC1, cv::Scalar(123)};
	}

	auto gray_is_sentinel(const cv::Mat &gray) -> bool {
		return gray.rows == 1 && gray.cols == 1 && gray.type() == CV_8UC1 &&
		       gray.at<unsigned char>(0, 0) == 123;
	}

	auto expect_rejected_frame(cv::Mat frame, const std::string &label) -> bool {
		auto context = std::make_shared<ReadContext>(std::move(frame));
		auto capture = howdy::native::VideoCaptureTestAccess::create(
		    howdy::native::CaptureSettings{.device_path = "none"}, context);

		cv::Mat    raw;
		cv::Mat    gray   = gray_sentinel();
		const bool result = capture.read(raw, &gray);

		bool ok = true;
		ok &= expect(!result, label + " is rejected");
		ok &= expect(capture.error() == howdy::native::CaptureError::kReadFailed,
		             label + " sets read-failed error");
		ok &= expect(context->calls == 1, label + " reads one test frame");
		ok &= expect(gray_is_sentinel(gray), label + " leaves gray output unchanged");
		return ok;
	}

	auto empty_frame_is_rejected_without_grayscale_conversion() -> bool {
		return expect_rejected_frame(cv::Mat(), "empty frame");
	}

	auto three_dimensional_frame_is_rejected_without_grayscale_conversion() -> bool {
		const std::array<int, 3> sizes{2, 2, 2};
		return expect_rejected_frame(cv::Mat(3, sizes.data(), CV_8UC1, cv::Scalar(0)), "3-D frame");
	}

	auto oversized_frame_is_rejected_without_grayscale_conversion() -> bool {
		return expect_rejected_frame(
		    cv::Mat(howdy::native::kMaxFrameDimension + 1, 1, CV_8UC1, cv::Scalar(0)),
		    "oversized frame");
	}

	auto unsupported_channel_frame_is_rejected_without_grayscale_conversion() -> bool {
		return expect_rejected_frame(cv::Mat(4, 4, CV_8UC2, cv::Scalar(0, 0)),
		                             "unsupported-channel frame");
	}

	auto non_8u_frame_is_rejected_without_grayscale_conversion() -> bool {
		return expect_rejected_frame(cv::Mat(4, 4, CV_32FC1, cv::Scalar(0.0F)), "non-CV_8U frame");
	}

	auto unconfigured_device_is_rejected_before_open() -> bool {
		auto capture =
		    howdy::native::VideoCapture(howdy::native::CaptureSettings{.device_path = "none"});
		const bool result = capture.open();
		bool       ok     = true;
		ok &= expect(capture.settings().device_path == "none",
		             "unconfigured camera keeps none sentinel");
		ok &= expect(!result, "unconfigured camera is rejected");
		ok &= expect(capture.error() == howdy::native::CaptureError::kMissingDevice,
		             "unconfigured camera sets missing-device error");
		ok &= expect(capture.error_message() == "Camera is not configured; set video.device_path",
		             "unconfigured camera error is concise and names config setting");
		return ok;
	}

	auto injected_reader_cannot_outlive_its_context() -> bool {
		std::weak_ptr<ReadContext> context_lifetime;
		bool                       ok = true;
		{
			auto context     = std::make_shared<ReadContext>(cv::Mat(2, 2, CV_8UC1, cv::Scalar(1)));
			context_lifetime = context;
			auto capture     = howdy::native::VideoCaptureTestAccess::create(
			    howdy::native::CaptureSettings{.device_path = "none"}, std::move(context));

			ok &= expect(!context_lifetime.expired(), "injected reader owns its context");
			cv::Mat frame;
			ok &= expect(capture.read(frame), "owned reader remains usable");
		}
		ok &= expect(context_lifetime.expired(), "injected reader is released with capture");
		return ok;
	}

}  // namespace

auto main() -> int {
	bool ok = true;
	ok &= empty_frame_is_rejected_without_grayscale_conversion();
	ok &= three_dimensional_frame_is_rejected_without_grayscale_conversion();
	ok &= oversized_frame_is_rejected_without_grayscale_conversion();
	ok &= unsupported_channel_frame_is_rejected_without_grayscale_conversion();
	ok &= non_8u_frame_is_rejected_without_grayscale_conversion();
	ok &= unconfigured_device_is_rejected_before_open();
	ok &= injected_reader_cannot_outlive_its_context();
	return ok ? 0 : 1;
}
