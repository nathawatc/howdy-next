#include "support/frame_limits.hpp"
#include "test_support.hpp"
#include "vision/video_capture.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include <opencv2/core.hpp>
#include <opencv2/core/base.hpp>
#include <opencv2/core/exception.hpp>
#include <opencv2/core/hal/interface.h>
#include <opencv2/core/mat.hpp>
#include <opencv2/core/types.hpp>

namespace howdy::native {

	class VideoCaptureTestAccess {
	public:
		class ReadContext final : public VideoCapture::FrameReader {
		public:
			enum class Behavior : std::uint8_t {
				kFrame,
				kFailure,
				kException
			};

			explicit ReadContext(cv::Mat source_frame, Behavior behavior = Behavior::kFrame)
			    : frame_(std::move(source_frame))
			    , behavior_(behavior) {}

			auto Read(cv::Mat &output) -> bool override {
				calls_++;
				if (behavior_ == Behavior::kException) {
					throw cv::Exception(cv::Error::StsError, "injected read failure", "Read",
					                    "video_capture_test.cpp", 1);
				}
				if (behavior_ == Behavior::kFailure) {
					return false;
				}
				output = frame_.clone();
				return true;
			}

			[[nodiscard]] auto Calls() const -> int {
				return calls_;
			}

		private:
			cv::Mat  frame_;
			Behavior behavior_ = Behavior::kFrame;
			int      calls_    = 0;
		};

		static auto Create(CaptureSettings settings, std::shared_ptr<ReadContext> frame_reader)
		    -> VideoCapture {
			return {std::move(settings), std::move(frame_reader)};
		}
	};

}  // namespace howdy::native

namespace {

	using howdy::test::Expect;

	using ReadContext = howdy::native::VideoCaptureTestAccess::ReadContext;

	auto GraySentinel() -> cv::Mat {
		return {1, 1, CV_8UC1, cv::Scalar(123)};
	}

	auto GrayIsSentinel(const cv::Mat &gray) -> bool {
		return gray.rows == 1 && gray.cols == 1 && gray.type() == CV_8UC1 &&
		       gray.at<unsigned char>(0, 0) == 123;
	}

	auto ExpectRejectedFrame(cv::Mat frame, const std::string &label) -> bool {
		auto context = std::make_shared<ReadContext>(std::move(frame));
		auto capture = howdy::native::VideoCaptureTestAccess::Create(
		    howdy::native::CaptureSettings{.device_path = "none"}, context);

		cv::Mat    raw;
		cv::Mat    gray   = GraySentinel();
		const bool result = capture.Read(raw, &gray);

		bool ok = true;
		ok &= Expect(!result, label + " is rejected");
		ok &= Expect(capture.Error() == howdy::native::CaptureError::kReadFailed,
		             label + " sets read-failed error");
		ok &= Expect(context->Calls() == 1, label + " reads one test frame");
		ok &= Expect(GrayIsSentinel(gray), label + " leaves gray output unchanged");
		return ok;
	}

	auto EmptyFrameIsRejectedWithoutGrayscaleConversion() -> bool {
		return ExpectRejectedFrame(cv::Mat(), "empty frame");
	}

	auto ThreeDimensionalFrameIsRejectedWithoutGrayscaleConversion() -> bool {
		const std::array<int, 3> sizes{2, 2, 2};
		return ExpectRejectedFrame(cv::Mat(3, sizes.data(), CV_8UC1, cv::Scalar(0)), "3-D frame");
	}

	auto OversizedFrameIsRejectedWithoutGrayscaleConversion() -> bool {
		return ExpectRejectedFrame(
		    cv::Mat(howdy::native::kMaxFrameDimension + 1, 1, CV_8UC1, cv::Scalar(0)),
		    "oversized frame");
	}

	auto UnsupportedChannelFrameIsRejectedWithoutGrayscaleConversion() -> bool {
		return ExpectRejectedFrame(cv::Mat(4, 4, CV_8UC2, cv::Scalar(0, 0)),
		                           "unsupported-channel frame");
	}

	auto Non8uFrameIsRejectedWithoutGrayscaleConversion() -> bool {
		return ExpectRejectedFrame(cv::Mat(4, 4, CV_32FC1, cv::Scalar(0.0F)), "non-CV_8U frame");
	}

	auto ColorFrameConvertsToGray(const cv::Mat &source, const std::string &label) -> bool {
		auto context = std::make_shared<ReadContext>(source);
		auto capture = howdy::native::VideoCaptureTestAccess::Create(
		    howdy::native::CaptureSettings{.device_path = "none"}, context);
		cv::Mat    raw;
		cv::Mat    gray;
		const bool read_ok = capture.Read(raw, &gray);
		bool       ok      = true;
		ok &= Expect(read_ok && capture.Error() == howdy::native::CaptureError::kNone,
		             label + " read succeeds");
		ok &= Expect(read_ok && raw.type() == source.type() && raw.size() == source.size() &&
		                 cv::norm(raw, source, cv::NORM_INF) == 0.0,
		             label + " preserves raw color frame");
		ok &= Expect(read_ok && gray.type() == CV_8UC1 && gray.rows == 1 && gray.cols == 1 &&
		                 gray.at<unsigned char>(0, 0) == 76,
		             label + " converts red pixel to grayscale");
		ok &= Expect(context->Calls() == 1, label + " reads once");
		return ok;
	}

	auto ReadFailureIsReported(ReadContext::Behavior behavior, const std::string &label) -> bool {
		auto context = std::make_shared<ReadContext>(cv::Mat(), behavior);
		auto capture = howdy::native::VideoCaptureTestAccess::Create(
		    howdy::native::CaptureSettings{.device_path = "none"}, context);
		cv::Mat    raw;
		cv::Mat    gray    = GraySentinel();
		const bool read_ok = capture.Read(raw, &gray);
		bool       ok      = true;
		ok &= Expect(!read_ok && capture.Error() == howdy::native::CaptureError::kReadFailed,
		             label + " fails closed");
		const bool expected_diagnostic =
		    behavior == ReadContext::Behavior::kFailure
		        ? capture.ErrorMessage() == howdy::native::kCameraReadFailureMessage
		        : capture.ErrorMessage().starts_with(
		              "OpenCV failed while reading camera frame: ") &&
		              capture.ErrorMessage().contains("injected read failure");
		ok &= Expect(expected_diagnostic, label + " reports read failure");
		ok &= Expect(GrayIsSentinel(gray), label + " leaves gray output unchanged");
		ok &= Expect(context->Calls() == 1, label + " reads once");
		return ok;
	}

	auto UnconfiguredDeviceIsRejectedBeforeOpen() -> bool {
		auto capture =
		    howdy::native::VideoCapture(howdy::native::CaptureSettings{.device_path = "none"});
		const bool result = capture.Open();
		bool       ok     = true;
		ok &= Expect(!result, "unconfigured camera is rejected");
		ok &= Expect(capture.Error() == howdy::native::CaptureError::kMissingDevice,
		             "unconfigured camera sets missing-device error");
		ok &= Expect(capture.ErrorMessage() == "Camera is not configured; set video.device_path",
		             "unconfigured camera error is concise and names config setting");
		return ok;
	}

	auto InjectedReaderCannotOutliveItsContext() -> bool {
		std::weak_ptr<ReadContext> context_lifetime;
		bool                       ok = true;
		{
			auto context     = std::make_shared<ReadContext>(cv::Mat(2, 2, CV_8UC1, cv::Scalar(1)));
			context_lifetime = context;
			auto capture     = howdy::native::VideoCaptureTestAccess::Create(
			    howdy::native::CaptureSettings{.device_path = "none"}, std::move(context));

			ok &= Expect(!context_lifetime.expired(), "injected reader owns its context");
			cv::Mat frame;
			ok &= Expect(capture.Read(frame), "owned reader remains usable");
		}
		ok &= Expect(context_lifetime.expired(), "injected reader is released with capture");
		return ok;
	}

}  // namespace

auto main() -> int {
	bool ok = true;
	ok &= EmptyFrameIsRejectedWithoutGrayscaleConversion();
	ok &= ThreeDimensionalFrameIsRejectedWithoutGrayscaleConversion();
	ok &= OversizedFrameIsRejectedWithoutGrayscaleConversion();
	ok &= UnsupportedChannelFrameIsRejectedWithoutGrayscaleConversion();
	ok &= Non8uFrameIsRejectedWithoutGrayscaleConversion();
	ok &= ColorFrameConvertsToGray(cv::Mat(1, 1, CV_8UC3, cv::Scalar(0, 0, 255)), "BGR");
	ok &= ColorFrameConvertsToGray(cv::Mat(1, 1, CV_8UC4, cv::Scalar(0, 0, 255, 17)), "BGRA");
	ok &= ReadFailureIsReported(ReadContext::Behavior::kFailure, "false read");
	ok &= ReadFailureIsReported(ReadContext::Behavior::kException, "OpenCV exception");
	ok &= UnconfiguredDeviceIsRejectedBeforeOpen();
	ok &= InjectedReaderCannotOutliveItsContext();
	return ok ? 0 : 1;
}
