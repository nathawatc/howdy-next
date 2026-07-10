#include "common/compare_capture_session.hpp"

#include <chrono>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>

namespace {
	int set_property_calls_without_context = 0;

	struct FakeCaptureContext {
		bool        open_result = true;
		bool        read_result = true;
		std::string error       = "synthetic capture failure";
		cv::Mat     next_gray_frame;

		int open_calls = 0;
		int read_calls = 0;

		struct PropertyCall {
			int    property;
			double value;
		};

		std::vector<PropertyCall> property_calls;
	};

	struct FakeClockContext {
		std::chrono::steady_clock::time_point now{};  // NOLINT(readability-redundant-member-init)
		int                                   calls = 0;
	};

	auto expect(bool condition, const std::string &message) -> bool {
		if (!condition) {
			std::cerr << "FAIL: " << message << "\n";
			return false;
		}
		return true;
	}

	auto fake_open(void *context) -> bool {
		auto &capture = *static_cast<FakeCaptureContext *>(context);
		capture.open_calls++;
		return capture.open_result;
	}

	auto fake_read_gray_frame(void *context, cv::Mat &gray_frame) -> bool {
		auto &capture = *static_cast<FakeCaptureContext *>(context);
		capture.read_calls++;
		if (!capture.read_result) {
			return false;
		}
		gray_frame = capture.next_gray_frame.clone();
		return true;
	}

	auto fake_error_message(void *context) -> std::string {
		return static_cast<FakeCaptureContext *>(context)->error;
	}

	auto fake_set_property(void *context, int property, double value) -> bool {
		auto &capture = *static_cast<FakeCaptureContext *>(context);
		capture.property_calls.push_back({
		    .property = property,
		    .value    = value,
		});
		return true;
	}

	auto fake_set_property_without_context([[maybe_unused]] void  *context,
	                                       [[maybe_unused]] int    property,
	                                       [[maybe_unused]] double value) -> bool {
		set_property_calls_without_context++;
		return true;
	}

	auto fake_now(void *context) -> std::chrono::steady_clock::time_point {
		auto &clock = *static_cast<FakeClockContext *>(context);
		clock.calls++;
		return clock.now;
	}

	auto make_dependencies(FakeCaptureContext &capture, FakeClockContext &clock)
	    -> howdy::native::CompareCaptureDependencies {
		return {
		    .capture_context = &capture,
		    .open_capture    = fake_open,
		    .read_gray_frame = fake_read_gray_frame,
		    .error_message   = fake_error_message,
		    .set_property    = fake_set_property,
		    .clock_context   = &clock,
		    .now             = fake_now,
		};
	}

	auto make_video_config() -> howdy::native::VideoConfig {
		return {
		    .timeout              = 2,
		    .device_path          = "dummy",
		    .warn_no_device       = false,
		    .max_height           = 320.0F,
		    .frame_width          = 640,
		    .frame_height         = 480,
		    .clahe_enabled        = false,
		    .clahe_clip_limit     = 2.0F,
		    .clahe_tile_grid_size = 8,
		    .dark_threshold       = 50.0F,
		    .force_mjpeg          = false,
		    .exposure             = -1,
		    .device_fps           = 30,
		    .rotate               = 0,
		};
	}

	auto matrix_equal(const cv::Mat &actual, const cv::Mat &expected) -> bool {
		return actual.size() == expected.size() && actual.type() == expected.type() &&
		       cv::countNonZero(actual != expected) == 0;
	}

}  // namespace

auto main() -> int {
	using namespace std::chrono_literals;
	using howdy::native::CompareCaptureFrameStatus;
	using howdy::native::CompareCaptureOpenStatus;

	bool ok = true;

	{
		FakeCaptureContext capture;
		FakeClockContext   clock;
		auto               dependencies = make_dependencies(capture, clock);
		dependencies.read_gray_frame    = nullptr;
		howdy::native::CompareCaptureSession session(make_video_config(), dependencies);

		const auto result = session.open();
		ok &= expect(result.status == CompareCaptureOpenStatus::kInvalidDependencies,
		             "invalid dependencies reject open");
		ok &= expect(capture.open_calls == 0, "invalid dependencies do not call open");
		ok &= expect(capture.read_calls == 0, "invalid dependencies do not call read");
		ok &= expect(clock.calls == 0, "invalid dependencies do not call clock");
	}

	{
		FakeCaptureContext capture;
		FakeClockContext   clock;
		capture.open_result = false;
		howdy::native::CompareCaptureSession session(make_video_config(),
		                                             make_dependencies(capture, clock));

		const auto result = session.open();
		ok &= expect(result.status == CompareCaptureOpenStatus::kOpenFailed,
		             "open failure has open-failed status");
		ok &= expect(result.error_message == "synthetic capture failure",
		             "open failure preserves error message");
		ok &= expect(capture.open_calls == 1, "open failure calls open once");
		ok &= expect(clock.calls == 0, "open failure does not start timer");
	}

	{
		FakeCaptureContext                   capture;
		FakeClockContext                     clock;
		howdy::native::CompareCaptureSession session(make_video_config(),
		                                             make_dependencies(capture, clock));

		ok &= expect(session.open().status == CompareCaptureOpenStatus::kOk,
		             "reopen-failure session opens initially");
		capture.open_result = false;
		ok &= expect(session.open().status == CompareCaptureOpenStatus::kOpenFailed,
		             "failed reopen has open-failed status");
		const auto result = session.next_frame();
		ok &= expect(result.status == CompareCaptureFrameStatus::kNotOpen,
		             "failed reopen leaves session closed");
		ok &= expect(capture.read_calls == 0, "failed reopen prevents additional read");
	}

	{
		FakeCaptureContext capture;
		FakeClockContext   clock;
		capture.next_gray_frame   = cv::Mat_<unsigned char>({2, 3}, {2, 7, 1, 8, 2, 8});
		auto dependencies         = make_dependencies(capture, clock);
		dependencies.set_property = nullptr;
		howdy::native::CompareCaptureSession session(make_video_config(), dependencies);

		ok &= expect(session.open().status == CompareCaptureOpenStatus::kOk,
		             "disabled exposure does not require property setter");
		const auto result = session.next_frame();
		ok &= expect(result.status == CompareCaptureFrameStatus::kFrameReady,
		             "disabled exposure reads without property setter");
		ok &= expect(capture.open_calls == 1, "missing optional setter still opens capture");
		ok &= expect(capture.read_calls == 1, "missing optional setter still reads capture");
	}

	{
		FakeCaptureContext capture;
		FakeClockContext   clock;
		auto               config       = make_video_config();
		auto               dependencies = make_dependencies(capture, clock);
		config.exposure                 = 37;
		dependencies.set_property       = nullptr;
		howdy::native::CompareCaptureSession session(config, dependencies);

		ok &= expect(session.open().status == CompareCaptureOpenStatus::kInvalidDependencies,
		             "configured exposure requires property setter");
		session.restore_exposure();
		ok &= expect(capture.open_calls == 0, "missing required setter prevents capture open");
		ok &= expect(clock.calls == 0, "missing required setter does not start clock");
		ok &= expect(capture.property_calls.empty(),
		             "missing required setter returns safely during exposure restore");
	}

	{
		FakeCaptureContext                   capture;
		FakeClockContext                     clock;
		howdy::native::CompareCaptureSession session(make_video_config(),
		                                             make_dependencies(capture, clock));

		const auto result = session.next_frame();
		ok &= expect(result.status == CompareCaptureFrameStatus::kNotOpen,
		             "read before open has not-open status");
		ok &= expect(capture.read_calls == 0, "read before open does not call read");
		ok &= expect(session.stats().frames == 0, "read before open does not count frame");
	}

	{
		FakeCaptureContext                   capture;
		FakeClockContext                     clock;
		howdy::native::CompareCaptureSession session(make_video_config(),
		                                             make_dependencies(capture, clock));

		session.reset_timeout_clock();
		const auto &stats = session.stats();
		ok &= expect(clock.calls == 0, "timeout reset before open does not call clock");
		ok &= expect(stats.frames == 0 && stats.black_frames == 0 && stats.dark_frames == 0 &&
		                 stats.valid_frames == 0 && stats.dark_running_total == 0.0,
		             "timeout reset before open does not change statistics");
	}

	{
		FakeCaptureContext capture;
		FakeClockContext   clock;
		capture.next_gray_frame = cv::Mat_<unsigned char>({2, 3}, {1, 2, 3, 4, 5, 6});
		howdy::native::CompareCaptureSession session(make_video_config(),
		                                             make_dependencies(capture, clock));

		ok &= expect(session.open().status == CompareCaptureOpenStatus::kOk,
		             "timeout-boundary session opens");
		clock.now         = std::chrono::steady_clock::time_point{} + 2s;
		const auto result = session.next_frame();
		ok &= expect(result.status == CompareCaptureFrameStatus::kTimeout,
		             "exact timeout boundary times out");
		ok &= expect(capture.read_calls == 0, "exact timeout boundary does not call read");
		ok &= expect(result.frame_number == 1, "exact timeout boundary returns frame one");
	}

	{
		FakeCaptureContext capture;
		FakeClockContext   clock;
		capture.next_gray_frame = cv::Mat_<unsigned char>({2, 3}, {8, 6, 7, 5, 3, 0});
		howdy::native::CompareCaptureSession session(make_video_config(),
		                                             make_dependencies(capture, clock));

		ok &= expect(session.open().status == CompareCaptureOpenStatus::kOk,
		             "timeout-reset session opens");
		clock.now = std::chrono::steady_clock::time_point{} + 2s;
		session.reset_timeout_clock();
		ok &= expect(session.stats().frames == 0 && session.stats().black_frames == 0 &&
		                 session.stats().dark_frames == 0 && session.stats().valid_frames == 0 &&
		                 session.stats().dark_running_total == 0.0,
		             "timeout reset does not change statistics");

		clock.now        = std::chrono::steady_clock::time_point{} + 4s;
		const auto ready = session.next_frame();
		ok &= expect(ready.status == CompareCaptureFrameStatus::kTimeout,
		             "reset timeout boundary times out");
		ok &= expect(ready.frame_number == 1, "timeout after reset is frame one");

		clock.now          = std::chrono::steady_clock::time_point{} + 5s;
		const auto timeout = session.next_frame();
		ok &= expect(timeout.status == CompareCaptureFrameStatus::kTimeout,
		             "elapsed time after reset boundary times out");
		ok &= expect(timeout.frame_number == 2, "second timeout after reset is frame two");
		ok &= expect(capture.read_calls == 0, "timeout reset sequence never reads");
		ok &= expect(capture.property_calls.empty(),
		             "timeout reset sequence does not set capture properties");
	}

	{
		FakeCaptureContext                   capture;
		FakeClockContext                     clock;
		howdy::native::CompareCaptureSession session(make_video_config(),
		                                             make_dependencies(capture, clock));

		ok &=
		    expect(session.open().status == CompareCaptureOpenStatus::kOk, "timeout session opens");
		clock.now         = std::chrono::steady_clock::time_point{} + 3s;
		const auto result = session.next_frame();
		ok &= expect(result.status == CompareCaptureFrameStatus::kTimeout,
		             "elapsed time above timeout times out");
		ok &= expect(result.frame_number == 1, "timeout returns incremented frame number");
		ok &= expect(session.stats().frames == 1, "timeout increments frame statistics");
		ok &= expect(capture.read_calls == 0, "timeout does not call read");
	}

	{
		FakeCaptureContext capture;
		FakeClockContext   clock;
		capture.read_result = false;
		howdy::native::CompareCaptureSession session(make_video_config(),
		                                             make_dependencies(capture, clock));

		ok &= expect(session.open().status == CompareCaptureOpenStatus::kOk,
		             "read-failure session opens");
		const auto result = session.next_frame();
		ok &= expect(result.status == CompareCaptureFrameStatus::kReadFailed,
		             "failed read has read-failed status");
		ok &= expect(result.error_message == "synthetic capture failure",
		             "read failure preserves error message");
		ok &= expect(result.frame_number == 1, "read failure returns frame one");
		ok &= expect(capture.read_calls == 1, "read failure calls read once");
		ok &= expect(session.stats().frames == 1, "read failure increments frame statistics");
	}

	{
		FakeCaptureContext capture;
		FakeClockContext   clock;
		capture.next_gray_frame = cv::Mat_<unsigned char>({2, 3}, {3, 1, 4, 1, 5, 9});
		howdy::native::CompareCaptureSession session(make_video_config(),
		                                             make_dependencies(capture, clock));

		ok &= expect(session.open().status == CompareCaptureOpenStatus::kOk,
		             "successful-read session opens");
		const auto result = session.next_frame();
		ok &= expect(result.status == CompareCaptureFrameStatus::kFrameReady,
		             "successful read returns frame-ready status");
		ok &= expect(matrix_equal(result.gray_frame, capture.next_gray_frame),
		             "successful read returns grayscale copy");
		ok &= expect(result.frame_number == 1, "successful read returns frame one");
		ok &= expect(result.gray_frame.data != capture.next_gray_frame.data,
		             "returned grayscale frame does not alias source");
		ok &= expect(session.stats().frames == 1, "successful read increments frame statistics");
	}

	{
		FakeCaptureContext                   capture;
		FakeClockContext                     clock;
		howdy::native::CompareCaptureSession session(make_video_config(),
		                                             make_dependencies(capture, clock));

		ok &= expect(session.open().status == CompareCaptureOpenStatus::kOk,
		             "statistics session opens");
		session.record_black_frame();
		session.record_dark_frame(12.5F);
		session.record_ready_frame(3.25F);

		const auto &stats = session.stats();
		ok &= expect(stats.frames == 0, "classification statistics do not count capture frames");
		ok &= expect(stats.black_frames == 1, "black-frame statistic increments");
		ok &= expect(stats.dark_frames == 1, "dark-frame statistic increments");
		ok &= expect(stats.valid_frames == 2, "dark and ready frames count as valid");
		ok &= expect(std::fabs(stats.dark_running_total - 15.75) < 0.0001,
		             "darkness total includes dark and ready frames");
	}

	{
		FakeCaptureContext capture;
		FakeClockContext   clock;
		auto               config = make_video_config();
		config.exposure           = 37;
		howdy::native::CompareCaptureSession session(config, make_dependencies(capture, clock));

		ok &= expect(session.open().status == CompareCaptureOpenStatus::kOk,
		             "configured-exposure session opens");
		session.restore_exposure();
		ok &= expect(capture.property_calls.size() == 2,
		             "configured exposure makes exactly two property calls");
		if (capture.property_calls.size() == 2) {
			ok &= expect(capture.property_calls[0].property == cv::CAP_PROP_AUTO_EXPOSURE &&
			                 capture.property_calls[0].value == 1.0,
			             "auto exposure is restored first");
			ok &= expect(capture.property_calls[1].property == cv::CAP_PROP_EXPOSURE &&
			                 capture.property_calls[1].value == 37.0,
			             "explicit exposure is restored second");
		}
	}

	{
		FakeCaptureContext capture;
		FakeClockContext   clock;
		auto               config          = make_video_config();
		auto               dependencies    = make_dependencies(capture, clock);
		config.exposure                    = 37;
		dependencies.capture_context       = nullptr;
		dependencies.set_property          = fake_set_property_without_context;
		set_property_calls_without_context = 0;
		howdy::native::CompareCaptureSession session(config, dependencies);

		session.restore_exposure();
		ok &= expect(set_property_calls_without_context == 0,
		             "invalid capture context does not invoke property setter");
	}

	{
		FakeCaptureContext                   capture;
		FakeClockContext                     clock;
		howdy::native::CompareCaptureSession session(make_video_config(),
		                                             make_dependencies(capture, clock));

		session.restore_exposure();
		ok &= expect(capture.property_calls.empty(), "disabled exposure makes no property calls");
	}

	return ok ? 0 : 1;
}
