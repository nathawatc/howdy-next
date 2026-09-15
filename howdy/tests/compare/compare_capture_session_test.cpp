#include "compare/capture_session.hpp"
#include "test_support.hpp"

#include <chrono>
#include <cmath>
#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>

namespace {

	using howdy::test::Expect;
	int set_property_calls_without_context = 0;

	struct FakeCaptureContext {
		bool        open_result = true;
		bool        read_result = true;
		std::string error       = "synthetic capture failure";
		cv::Mat     next_gray_frame;

		int  open_calls             = 0;
		int  read_calls             = 0;
		int  property_call_count    = 0;
		int  warm_up_calls          = 0;
		int  throw_on_property_call = 0;
		bool set_property_result    = true;
		bool warm_up_result         = true;

		struct PropertyCall {
			int    property;
			double value;
		};

		std::vector<PropertyCall> property_calls;
		std::vector<std::string>  events;
	};

	struct FakeClockContext {
		std::chrono::steady_clock::time_point now{};  // NOLINT(readability-redundant-member-init)
		int                                   calls = 0;
	};

	auto FakeOpen(void *context) -> bool {
		auto &capture = *static_cast<FakeCaptureContext *>(context);
		capture.open_calls++;
		capture.events.emplace_back("open");
		return capture.open_result;
	}

	auto FakeWarmUp(void *context) -> bool {
		auto &capture = *static_cast<FakeCaptureContext *>(context);
		capture.warm_up_calls++;
		capture.events.emplace_back("warm-up");
		return capture.warm_up_result;
	}

	auto FakeReadGrayFrame(void *context, cv::Mat &gray_frame) -> bool {
		auto &capture = *static_cast<FakeCaptureContext *>(context);
		capture.read_calls++;
		capture.events.emplace_back("read");
		if (!capture.read_result) {
			return false;
		}
		gray_frame = capture.next_gray_frame.clone();
		return true;
	}

	auto FakeErrorMessage(void *context) -> std::string {
		return static_cast<FakeCaptureContext *>(context)->error;
	}

	auto FakeSetProperty(void *context, howdy::native::CaptureProperty property) -> bool {
		auto &capture = *static_cast<FakeCaptureContext *>(context);
		capture.property_calls.push_back({
		    .property = property.id,
		    .value    = property.value,
		});
		capture.events.emplace_back(property.id == cv::CAP_PROP_AUTO_EXPOSURE ? "auto-exposure"
		                                                                      : "exposure");
		capture.property_call_count++;
		if (capture.property_call_count == capture.throw_on_property_call) {
			throw cv::Exception(cv::Error::StsError, "synthetic property failure",
			                    "fake_set_property", __FILE__, __LINE__);
		}
		return capture.set_property_result;
	}

	auto FakeSetPropertyWithoutContext([[maybe_unused]] void                          *context,
	                                   [[maybe_unused]] howdy::native::CaptureProperty property)
	    -> bool {
		set_property_calls_without_context++;
		return true;
	}

	auto FakeNow(void *context) -> std::chrono::steady_clock::time_point {
		auto &clock = *static_cast<FakeClockContext *>(context);
		clock.calls++;
		return clock.now;
	}

	auto MakeDependencies(FakeCaptureContext &capture, FakeClockContext &clock)
	    -> howdy::native::CompareCaptureDependencies {
		return {
		    .capture_context = &capture,
		    .open_capture    = FakeOpen,
		    .warm_up_capture = FakeWarmUp,
		    .read_gray_frame = FakeReadGrayFrame,
		    .error_message   = FakeErrorMessage,
		    .set_property    = FakeSetProperty,
		    .clock_context   = &clock,
		    .now             = FakeNow,
		};
	}

	auto MakeVideoConfig() -> howdy::native::VideoConfig {
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

	auto MatrixEqual(const cv::Mat &actual, const cv::Mat &expected) -> bool {
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
		auto               dependencies = MakeDependencies(capture, clock);
		dependencies.read_gray_frame    = nullptr;
		howdy::native::CompareCaptureSession session(MakeVideoConfig(), dependencies);

		const auto result = session.Open();
		ok &= Expect(result.status == CompareCaptureOpenStatus::kInvalidDependencies,
		             "invalid dependencies reject open");
		ok &= Expect(capture.open_calls == 0, "invalid dependencies do not call open");
		ok &= Expect(capture.read_calls == 0, "invalid dependencies do not call read");
		ok &= Expect(clock.calls == 0, "invalid dependencies do not call clock");
	}

	{
		FakeCaptureContext capture;
		FakeClockContext   clock;
		capture.open_result = false;
		howdy::native::CompareCaptureSession session(MakeVideoConfig(),
		                                             MakeDependencies(capture, clock));

		const auto result = session.Open();
		ok &= Expect(result.status == CompareCaptureOpenStatus::kOpenFailed,
		             "open failure has open-failed status");
		ok &= Expect(result.error_message == "synthetic capture failure",
		             "open failure preserves error message");
		ok &= Expect(capture.open_calls == 1, "open failure calls open once");
		ok &= Expect(clock.calls == 0, "open failure does not start timer");
	}

	{
		FakeCaptureContext capture;
		FakeClockContext   clock;
		capture.warm_up_result = false;
		howdy::native::CompareCaptureSession session(MakeVideoConfig(),
		                                             MakeDependencies(capture, clock));

		const auto result = session.Open();
		ok &= Expect(capture.open_calls == 1, "warm-up failure opens capture first");
		ok &= Expect(capture.events == std::vector<std::string>{"open", "warm-up"},
		             "warm-up failure occurs after capture open");
		ok &= Expect(result.status == CompareCaptureOpenStatus::kOpenFailed,
		             "warm-up failure has open-failed status");
		ok &= Expect(result.error_message == "synthetic capture failure",
		             "warm-up failure preserves error message");
		ok &= Expect(clock.calls == 0, "warm-up failure does not start timeout clock");
		const auto next = session.NextFrame();
		ok &= Expect(next.status == CompareCaptureFrameStatus::kNotOpen,
		             "warm-up failure leaves session closed");
		ok &= Expect(capture.read_calls == 0, "warm-up failure prevents frame reads");
	}

	{
		FakeCaptureContext                   capture;
		FakeClockContext                     clock;
		howdy::native::CompareCaptureSession session(MakeVideoConfig(),
		                                             MakeDependencies(capture, clock));

		ok &= Expect(session.Open().status == CompareCaptureOpenStatus::kOk,
		             "reopen-failure session opens initially");
		capture.open_result = false;
		ok &= Expect(session.Open().status == CompareCaptureOpenStatus::kOpenFailed,
		             "failed reopen has open-failed status");
		const auto result = session.NextFrame();
		ok &= Expect(result.status == CompareCaptureFrameStatus::kNotOpen,
		             "failed reopen leaves session closed");
		ok &= Expect(capture.read_calls == 0, "failed reopen prevents additional read");
	}

	{
		FakeCaptureContext capture;
		FakeClockContext   clock;
		capture.next_gray_frame   = cv::Mat_<unsigned char>({2, 3}, {2, 7, 1, 8, 2, 8});
		auto dependencies         = MakeDependencies(capture, clock);
		dependencies.set_property = nullptr;
		howdy::native::CompareCaptureSession session(MakeVideoConfig(), dependencies);

		ok &= Expect(session.Open().status == CompareCaptureOpenStatus::kOk,
		             "disabled exposure does not require property setter");
		const auto result = session.NextFrame();
		ok &= Expect(result.status == CompareCaptureFrameStatus::kFrameReady,
		             "disabled exposure reads without property setter");
		ok &= Expect(capture.open_calls == 1, "missing optional setter still opens capture");
		ok &= Expect(capture.read_calls == 1, "missing optional setter still reads capture");
	}

	{
		FakeCaptureContext capture;
		FakeClockContext   clock;
		auto               config       = MakeVideoConfig();
		auto               dependencies = MakeDependencies(capture, clock);
		config.exposure                 = 37;
		dependencies.set_property       = nullptr;
		howdy::native::CompareCaptureSession session(config, dependencies);

		ok &= Expect(session.Open().status == CompareCaptureOpenStatus::kInvalidDependencies,
		             "configured exposure requires property setter");
		session.RestoreExposure();
		ok &= Expect(capture.open_calls == 0, "missing required setter prevents capture open");
		ok &= Expect(clock.calls == 0, "missing required setter does not start clock");
		ok &= Expect(capture.property_calls.empty(),
		             "missing required setter returns safely during exposure restore");
	}

	{
		FakeCaptureContext                   capture;
		FakeClockContext                     clock;
		howdy::native::CompareCaptureSession session(MakeVideoConfig(),
		                                             MakeDependencies(capture, clock));

		const auto result = session.NextFrame();
		ok &= Expect(result.status == CompareCaptureFrameStatus::kNotOpen,
		             "read before open has not-open status");
		ok &= Expect(capture.read_calls == 0, "read before open does not call read");
		ok &= Expect(session.Stats().frames == 0, "read before open does not count frame");
	}

	{
		FakeCaptureContext                   capture;
		FakeClockContext                     clock;
		howdy::native::CompareCaptureSession session(MakeVideoConfig(),
		                                             MakeDependencies(capture, clock));

		session.ResetTimeoutClock();
		const auto &stats = session.Stats();
		ok &= Expect(clock.calls == 0, "timeout reset before open does not call clock");
		ok &= Expect(stats.frames == 0 && stats.black_frames == 0 && stats.dark_frames == 0 &&
		                 stats.valid_frames == 0 && stats.dark_running_total == 0.0,
		             "timeout reset before open does not change statistics");
	}

	{
		FakeCaptureContext capture;
		FakeClockContext   clock;
		capture.next_gray_frame = cv::Mat_<unsigned char>({2, 3}, {1, 2, 3, 4, 5, 6});
		howdy::native::CompareCaptureSession session(MakeVideoConfig(),
		                                             MakeDependencies(capture, clock));

		ok &= Expect(session.Open().status == CompareCaptureOpenStatus::kOk,
		             "timeout-boundary session opens");
		clock.now         = std::chrono::steady_clock::time_point{} + 2s;
		const auto result = session.NextFrame();
		ok &= Expect(result.status == CompareCaptureFrameStatus::kTimeout,
		             "exact timeout boundary times out");
		ok &= Expect(capture.read_calls == 0, "exact timeout boundary does not call read");
		ok &= Expect(result.frame_number == 1, "exact timeout boundary returns frame one");
	}

	{
		FakeCaptureContext capture;
		FakeClockContext   clock;
		capture.next_gray_frame = cv::Mat_<unsigned char>({2, 3}, {8, 6, 7, 5, 3, 0});
		howdy::native::CompareCaptureSession session(MakeVideoConfig(),
		                                             MakeDependencies(capture, clock));

		ok &= Expect(session.Open().status == CompareCaptureOpenStatus::kOk,
		             "timeout-reset session opens");
		clock.now = std::chrono::steady_clock::time_point{} + 2s;
		session.ResetTimeoutClock();
		ok &= Expect(session.Stats().frames == 0 && session.Stats().black_frames == 0 &&
		                 session.Stats().dark_frames == 0 && session.Stats().valid_frames == 0 &&
		                 session.Stats().dark_running_total == 0.0,
		             "timeout reset does not change statistics");

		clock.now        = std::chrono::steady_clock::time_point{} + 4s;
		const auto ready = session.NextFrame();
		ok &= Expect(ready.status == CompareCaptureFrameStatus::kTimeout,
		             "reset timeout boundary times out");
		ok &= Expect(ready.frame_number == 1, "timeout after reset is frame one");

		clock.now          = std::chrono::steady_clock::time_point{} + 5s;
		const auto timeout = session.NextFrame();
		ok &= Expect(timeout.status == CompareCaptureFrameStatus::kTimeout,
		             "elapsed time after reset boundary times out");
		ok &= Expect(timeout.frame_number == 2, "second timeout after reset is frame two");
		ok &= Expect(capture.read_calls == 0, "timeout reset sequence never reads");
		ok &= Expect(capture.property_calls.empty(),
		             "timeout reset sequence does not set capture properties");
	}

	{
		FakeCaptureContext                   capture;
		FakeClockContext                     clock;
		howdy::native::CompareCaptureSession session(MakeVideoConfig(),
		                                             MakeDependencies(capture, clock));

		ok &=
		    Expect(session.Open().status == CompareCaptureOpenStatus::kOk, "timeout session opens");
		clock.now         = std::chrono::steady_clock::time_point{} + 3s;
		const auto result = session.NextFrame();
		ok &= Expect(result.status == CompareCaptureFrameStatus::kTimeout,
		             "elapsed time above timeout times out");
		ok &= Expect(result.frame_number == 1, "timeout returns incremented frame number");
		ok &= Expect(session.Stats().frames == 1, "timeout increments frame statistics");
		ok &= Expect(capture.read_calls == 0, "timeout does not call read");
	}

	{
		FakeCaptureContext capture;
		FakeClockContext   clock;
		capture.read_result = false;
		howdy::native::CompareCaptureSession session(MakeVideoConfig(),
		                                             MakeDependencies(capture, clock));

		ok &= Expect(session.Open().status == CompareCaptureOpenStatus::kOk,
		             "read-failure session opens");
		const auto result = session.NextFrame();
		ok &= Expect(result.status == CompareCaptureFrameStatus::kReadFailed,
		             "failed read has read-failed status");
		ok &= Expect(result.error_message == "synthetic capture failure",
		             "read failure preserves error message");
		ok &= Expect(result.frame_number == 1, "read failure returns frame one");
		ok &= Expect(capture.read_calls == 1, "read failure calls read once");
		ok &= Expect(session.Stats().frames == 1, "read failure increments frame statistics");
	}

	{
		FakeCaptureContext capture;
		FakeClockContext   clock;
		capture.next_gray_frame = cv::Mat_<unsigned char>({2, 3}, {3, 1, 4, 1, 5, 9});
		howdy::native::CompareCaptureSession session(MakeVideoConfig(),
		                                             MakeDependencies(capture, clock));

		ok &= Expect(session.Open().status == CompareCaptureOpenStatus::kOk,
		             "successful-read session opens");
		const auto result = session.NextFrame();
		ok &= Expect(result.status == CompareCaptureFrameStatus::kFrameReady,
		             "successful read returns frame-ready status");
		ok &= Expect(MatrixEqual(result.gray_frame, capture.next_gray_frame),
		             "successful read returns grayscale copy");
		ok &= Expect(result.frame_number == 1, "successful read returns frame one");
		ok &= Expect(result.gray_frame.data != capture.next_gray_frame.data,
		             "returned grayscale frame does not alias source");
		ok &= Expect(session.Stats().frames == 1, "successful read increments frame statistics");
	}

	{
		FakeCaptureContext                   capture;
		FakeClockContext                     clock;
		howdy::native::CompareCaptureSession session(MakeVideoConfig(),
		                                             MakeDependencies(capture, clock));

		ok &= Expect(session.Open().status == CompareCaptureOpenStatus::kOk,
		             "statistics session opens");
		session.RecordBlackFrame();
		session.RecordDarkFrame(12.5F);
		session.RecordReadyFrame(3.25F);

		const auto &stats = session.Stats();
		ok &= Expect(stats.frames == 0, "classification statistics do not count capture frames");
		ok &= Expect(stats.black_frames == 1, "black-frame statistic increments");
		ok &= Expect(stats.dark_frames == 1, "dark-frame statistic increments");
		ok &= Expect(stats.valid_frames == 2, "dark and ready frames count as valid");
		ok &= Expect(std::fabs(stats.dark_running_total - 15.75) < 0.0001,
		             "darkness total includes dark and ready frames");
	}

	{
		FakeCaptureContext capture;
		FakeClockContext   clock;
		auto               config = MakeVideoConfig();
		config.exposure           = 37;
		howdy::native::CompareCaptureSession session(config, MakeDependencies(capture, clock));

		ok &= Expect(session.Open().status == CompareCaptureOpenStatus::kOk,
		             "configured-exposure session opens");
		ok &= Expect(capture.events ==
		                 std::vector<std::string>{"open", "auto-exposure", "exposure", "warm-up"},
		             "configured exposure is applied before warm-up");
		ok &= Expect(capture.warm_up_calls == 1, "configured exposure warms up once");
		ok &= Expect(capture.property_calls.size() == 2,
		             "configured exposure makes two initial property calls");

		session.RestoreExposure();
		ok &= Expect(capture.property_calls.size() == 4,
		             "configured exposure restoration makes two more property calls");
		ok &= Expect(capture.events == std::vector<std::string>{"open", "auto-exposure", "exposure",
		                                                        "warm-up", "auto-exposure",
		                                                        "exposure"},
		             "exposure restoration follows warm-up");
		if (capture.property_calls.size() == 4) {
			ok &= Expect(capture.property_calls[0].property == cv::CAP_PROP_AUTO_EXPOSURE &&
			                 capture.property_calls[0].value == 1.0,
			             "auto exposure is applied first");
			ok &= Expect(capture.property_calls[1].property == cv::CAP_PROP_EXPOSURE &&
			                 capture.property_calls[1].value == 37.0,
			             "explicit exposure is applied second");
			ok &= Expect(capture.property_calls[2].property == cv::CAP_PROP_AUTO_EXPOSURE &&
			                 capture.property_calls[2].value == 1.0,
			             "auto exposure is restored first");
			ok &= Expect(capture.property_calls[3].property == cv::CAP_PROP_EXPOSURE &&
			                 capture.property_calls[3].value == 37.0,
			             "explicit exposure is restored second");
		}
	}

	{
		FakeCaptureContext capture;
		FakeClockContext   clock;
		auto               config      = MakeVideoConfig();
		config.exposure                = 37;
		auto dependencies              = MakeDependencies(capture, clock);
		capture.throw_on_property_call = 1;
		howdy::native::CompareCaptureSession session(config, dependencies);

		const auto result = session.Open();
		ok &= Expect(result.status == CompareCaptureOpenStatus::kOk,
		             "initial property exception does not abort open");
		ok &= Expect(capture.events ==
		                 std::vector<std::string>{"open", "auto-exposure", "exposure", "warm-up"},
		             "initial property exception does not skip warm-up");
		session.RestoreExposure();
		ok &= Expect(capture.property_calls.size() == 4,
		             "initial property exception preserves later restoration");
	}

	{
		FakeCaptureContext capture;
		FakeClockContext   clock;
		auto               config      = MakeVideoConfig();
		config.exposure                = 37;
		auto dependencies              = MakeDependencies(capture, clock);
		capture.throw_on_property_call = 3;
		howdy::native::CompareCaptureSession session(config, dependencies);

		ok &= Expect(session.Open().status == CompareCaptureOpenStatus::kOk,
		             "first property exception session opens");
		bool restore_threw = false;
		try {
			session.RestoreExposure();
		} catch (const cv::Exception &) {
			restore_threw = true;
		}
		ok &= Expect(!restore_threw, "first exposure restoration exception is contained");
		ok &= Expect(capture.property_calls.size() == 4,
		             "second exposure restoration is attempted after first exception");
	}

	{
		FakeCaptureContext capture;
		FakeClockContext   clock;
		auto               config      = MakeVideoConfig();
		config.exposure                = 37;
		auto dependencies              = MakeDependencies(capture, clock);
		capture.throw_on_property_call = 4;
		howdy::native::CompareCaptureSession session(config, dependencies);

		ok &= Expect(session.Open().status == CompareCaptureOpenStatus::kOk,
		             "second property exception session opens");
		bool restore_threw = false;
		try {
			session.RestoreExposure();
		} catch (const cv::Exception &) {
			restore_threw = true;
		}
		ok &= Expect(!restore_threw, "second exposure restoration exception is contained");
		ok &= Expect(capture.property_calls.size() == 4,
		             "second exposure restoration call is attempted");
	}

	{
		FakeCaptureContext capture;
		FakeClockContext   clock;
		auto               config   = MakeVideoConfig();
		config.exposure             = 37;
		auto dependencies           = MakeDependencies(capture, clock);
		capture.set_property_result = false;
		howdy::native::CompareCaptureSession session(config, dependencies);

		ok &= Expect(session.Open().status == CompareCaptureOpenStatus::kOk,
		             "false property result session opens");
		session.RestoreExposure();
		ok &= Expect(capture.property_calls.size() == 4,
		             "false property results preserve best-effort application and restoration");
	}

	{
		FakeCaptureContext capture;
		FakeClockContext   clock;
		auto               config          = MakeVideoConfig();
		auto               dependencies    = MakeDependencies(capture, clock);
		config.exposure                    = 37;
		dependencies.capture_context       = nullptr;
		dependencies.set_property          = FakeSetPropertyWithoutContext;
		set_property_calls_without_context = 0;
		howdy::native::CompareCaptureSession session(config, dependencies);

		session.RestoreExposure();
		ok &= Expect(set_property_calls_without_context == 0,
		             "invalid capture context does not invoke property setter");
	}

	{
		FakeCaptureContext                   capture;
		FakeClockContext                     clock;
		howdy::native::CompareCaptureSession session(MakeVideoConfig(),
		                                             MakeDependencies(capture, clock));

		ok &= Expect(session.Open().status == CompareCaptureOpenStatus::kOk,
		             "disabled exposure session opens");
		ok &= Expect(capture.events == std::vector<std::string>{"open", "warm-up"},
		             "disabled exposure leaves camera properties untouched");
		session.RestoreExposure();
		ok &= Expect(capture.property_calls.empty(), "disabled exposure makes no property calls");
	}

	return ok ? 0 : 1;
}
