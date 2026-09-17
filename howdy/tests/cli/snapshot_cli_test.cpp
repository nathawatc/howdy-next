#include "cli/snapshot/internal.hpp"
#include "test_support.hpp"

#include <bit>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

namespace {

	using howdy::test::Expect;

	constexpr std::size_t kSnapshotFrameCount = 4;

	struct StreamRedirect {
		StreamRedirect(std::ostream &stream, std::streambuf *new_buffer)
		    : output(stream)
		    , old_buffer(stream.rdbuf(new_buffer)) {}

		~StreamRedirect() {
			output.rdbuf(old_buffer);
		}

		std::ostream   &output;
		std::streambuf *old_buffer;
	};

	struct SnapshotCliTestContext {
		howdy::native::RuntimeConfigLoadResult                  config_result;
		howdy::native::snapshot_internal::SnapshotCaptureResult capture_result;
		howdy::native::snapshot_internal::SnapshotWriteResult   write_result;
		int                                                     load_calls    = 0;
		int                                                     capture_calls = 0;
		int                                                     write_calls   = 0;
		howdy::native::RuntimeConfig                            capture_config;
		howdy::native::RuntimeConfig                            write_config;
		std::size_t                                             write_frame_count = 0;
	};

	auto ValidConfigLoadResult() -> howdy::native::RuntimeConfigLoadResult {
		howdy::native::RuntimeConfig config;
		config.video.dark_threshold = 41.0F;
		config.face.sface_threshold = 0.42F;
		return howdy::native::RuntimeConfigLoadResult{
		    .ok     = true,
		    .status = howdy::native::RuntimeConfigLoadStatus::kOk,
		    .config = config,
		};
	}

	auto InvalidConfigLoadResult() -> howdy::native::RuntimeConfigLoadResult {
		return howdy::native::RuntimeConfigLoadResult{
		    .ok            = false,
		    .status        = howdy::native::RuntimeConfigLoadStatus::kInvalidRuntimeValue,
		    .error_message = "invalid runtime config",
		};
	}

	auto DummyFrames() -> std::vector<cv::Mat> {
		return {
		    cv::Mat(1, 1, CV_8UC3, cv::Scalar(1, 2, 3)),
		    cv::Mat(1, 1, CV_8UC3, cv::Scalar(4, 5, 6)),
		    cv::Mat(1, 1, CV_8UC3, cv::Scalar(7, 8, 9)),
		    cv::Mat(1, 1, CV_8UC3, cv::Scalar(10, 11, 12)),
		};
	}

	auto SuccessfulCaptureResult() -> howdy::native::snapshot_internal::SnapshotCaptureResult {
		return howdy::native::snapshot_internal::SnapshotCaptureResult{
		    .status = howdy::native::snapshot_internal::SnapshotCaptureStatus::kOk,
		    .frames = DummyFrames(),
		};
	}

	auto LoadRuntimeConfigCallback(void *raw_context) -> howdy::native::RuntimeConfigLoadResult {
		auto *context = static_cast<SnapshotCliTestContext *>(raw_context);
		++context->load_calls;
		return context->config_result;
	}

	auto CaptureFramesCallback(void *raw_context, const howdy::native::RuntimeConfig &config)
	    -> howdy::native::snapshot_internal::SnapshotCaptureResult {
		auto *context = static_cast<SnapshotCliTestContext *>(raw_context);
		++context->capture_calls;
		context->capture_config = config;
		return context->capture_result;
	}

	auto WriteSnapshotCallback(void *raw_context, const std::vector<cv::Mat> &frames,
	                           const howdy::native::RuntimeConfig &config)
	    -> howdy::native::snapshot_internal::SnapshotWriteResult {
		auto *context = static_cast<SnapshotCliTestContext *>(raw_context);
		++context->write_calls;
		context->write_frame_count = frames.size();
		context->write_config      = config;
		return context->write_result;
	}

	auto TestDependencies(SnapshotCliTestContext &context)
	    -> howdy::native::snapshot_internal::SnapshotDependencies {
		return howdy::native::snapshot_internal::SnapshotDependencies{
		    .context             = &context,
		    .load_runtime_config = LoadRuntimeConfigCallback,
		    .capture_frames      = CaptureFramesCallback,
		    .write_snapshot      = WriteSnapshotCallback,
		};
	}

	auto
	RunSnapshotWithDependencies(howdy::native::snapshot_internal::SnapshotDependencies dependencies,
	                            const howdy::native::CommandInvocation &invocation) -> int {
		return howdy::native::snapshot_internal::SnapshotMainWithDependencies(invocation,
		                                                                      dependencies);
	}

	auto RunSnapshot(SnapshotCliTestContext                 &context,
	                 const howdy::native::CommandInvocation &invocation) -> int {
		return RunSnapshotWithDependencies(TestDependencies(context), invocation);
	}

	auto MakeSuccessContext() -> SnapshotCliTestContext {
		SnapshotCliTestContext context;
		context.config_result  = ValidConfigLoadResult();
		context.capture_result = SuccessfulCaptureResult();
		context.write_result   = howdy::native::snapshot_internal::SnapshotWriteResult{
		    .ok   = true,
		    .path = "/tmp/howdy-test/snapshots/test.jpg",
		};
		return context;
	}

	auto InvalidRuntimeConfigStopsBeforeCaptureAndWrite() -> bool {
		auto context          = MakeSuccessContext();
		context.config_result = InvalidConfigLoadResult();
		std::ostringstream error;
		StreamRedirect     error_redirect(std::cerr, error.rdbuf());

		const int result = RunSnapshot(context, {});

		bool ok = true;
		ok &= Expect(result == 1, "invalid runtime config returns 1");
		ok &= Expect(context.load_calls == 1, "invalid runtime config loads once");
		ok &= Expect(context.capture_calls == 0, "invalid runtime config skips capture");
		ok &= Expect(context.write_calls == 0, "invalid runtime config skips write");
		ok &= Expect(error.str().contains("invalid runtime config"),
		             "invalid runtime config writes config error");
		return ok;
	}

	auto CameraOpenFailureStopsBeforeWrite() -> bool {
		auto context           = MakeSuccessContext();
		context.capture_result = howdy::native::snapshot_internal::SnapshotCaptureResult{
		    .status        = howdy::native::snapshot_internal::SnapshotCaptureStatus::kOpenError,
		    .error_message = "Camera is not configured; set video.device_path",
		};
		std::ostringstream error;
		StreamRedirect     error_redirect(std::cerr, error.rdbuf());

		const int result = RunSnapshot(context, {});

		bool ok = true;
		ok &= Expect(result == 1, "camera open failure returns 1");
		ok &= Expect(context.load_calls == 1, "camera open failure loads once");
		ok &= Expect(context.capture_calls == 1, "camera open failure captures once");
		ok &= Expect(context.capture_config.video.device_path == "none",
		             "camera open failure uses default unconfigured device");
		ok &= Expect(context.write_calls == 0, "camera open failure skips write");
		ok &= Expect(error.str() == context.capture_result.error_message + "\n",
		             "camera open failure writes only concise error");
		return ok;
	}

	auto CameraReadFailureStopsBeforeWrite() -> bool {
		auto context           = MakeSuccessContext();
		context.capture_result = howdy::native::snapshot_internal::SnapshotCaptureResult{
		    .status = howdy::native::snapshot_internal::SnapshotCaptureStatus::kReadError,
		};
		std::ostringstream error;
		StreamRedirect     error_redirect(std::cerr, error.rdbuf());

		const int result = RunSnapshot(context, {});

		bool ok = true;
		ok &= Expect(result == 1, "camera read failure returns 1");
		ok &= Expect(context.load_calls == 1, "camera read failure loads once");
		ok &= Expect(context.capture_calls == 1, "camera read failure captures once");
		ok &= Expect(context.write_calls == 0, "camera read failure skips write");
		ok &= Expect(error.str().contains("Could not capture a camera frame"),
		             "camera read failure writes read error");
		return ok;
	}

	auto UnexpectedSuccessfulCaptureFrameCountStopsBeforeWrite(std::size_t        frame_count,
	                                                           const std::string &test_name)
	    -> bool {
		auto context = MakeSuccessContext();
		context.capture_result.frames.clear();
		const auto frames = DummyFrames();
		for (std::size_t index = 0; index < frame_count; ++index) {
			context.capture_result.frames.push_back(frames[index % frames.size()]);
		}
		std::ostringstream error;
		StreamRedirect     error_redirect(std::cerr, error.rdbuf());

		const int result = RunSnapshot(context, {});

		bool ok = true;
		ok &= Expect(result == 1, test_name + " returns 1");
		ok &= Expect(context.load_calls == 1, test_name + " loads once");
		ok &= Expect(context.capture_calls == 1, test_name + " captures once");
		ok &= Expect(context.write_calls == 0, test_name + " skips write");
		ok &= Expect(error.str().contains(
		                 "Internal error: snapshot capture returned unexpected frame count"),
		             test_name + " writes frame count error");
		return ok;
	}

	auto ZeroFrameSuccessfulCaptureStopsBeforeWrite() -> bool {
		return UnexpectedSuccessfulCaptureFrameCountStopsBeforeWrite(
		    0, "zero-frame successful capture");
	}

	auto ShortSuccessfulCaptureStopsBeforeWrite() -> bool {
		return UnexpectedSuccessfulCaptureFrameCountStopsBeforeWrite(kSnapshotFrameCount - 1,
		                                                             "short successful capture");
	}

	auto OversizedSuccessfulCaptureStopsBeforeWrite() -> bool {
		return UnexpectedSuccessfulCaptureFrameCountStopsBeforeWrite(
		    kSnapshotFrameCount + 1, "oversized successful capture");
	}

	auto UnknownCaptureStatusFailsClosed() -> bool {
		auto context = MakeSuccessContext();
		context.capture_result.status =
		    std::bit_cast<howdy::native::snapshot_internal::SnapshotCaptureStatus>(
		        std::uint8_t{UINT8_MAX});
		std::ostringstream error;
		StreamRedirect     error_redirect(std::cerr, error.rdbuf());

		const int result = RunSnapshot(context, {});

		bool ok = true;
		ok &= Expect(result == 1, "unknown capture status returns 1");
		ok &= Expect(context.load_calls == 1, "unknown capture status loads once");
		ok &= Expect(context.capture_calls == 1, "unknown capture status captures once");
		ok &= Expect(context.write_calls == 0, "unknown capture status skips write");
		ok &= Expect(error.str().contains("Internal error: unknown snapshot capture status"),
		             "unknown capture status writes internal error");
		return ok;
	}

	auto EmptyWritePathReturnsError() -> bool {
		auto context         = MakeSuccessContext();
		context.write_result = howdy::native::snapshot_internal::SnapshotWriteResult{
		    .ok = true,
		};
		std::ostringstream error;
		StreamRedirect     error_redirect(std::cerr, error.rdbuf());

		const int result = RunSnapshot(context, {});

		bool ok = true;
		ok &= Expect(result == 1, "empty write path returns 1");
		ok &= Expect(context.load_calls == 1, "empty write path loads once");
		ok &= Expect(context.capture_calls == 1, "empty write path captures once");
		ok &= Expect(context.write_calls == 1, "empty write path calls writer once");
		ok &= Expect(error.str().contains("Failed to write snapshot"),
		             "empty write path writes snapshot error");
		return ok;
	}

	auto WriteFailureReturnsError() -> bool {
		auto context         = MakeSuccessContext();
		context.write_result = howdy::native::snapshot_internal::SnapshotWriteResult{};
		std::ostringstream error;
		StreamRedirect     error_redirect(std::cerr, error.rdbuf());

		const int result = RunSnapshot(context, {});

		bool ok = true;
		ok &= Expect(result == 1, "write failure returns 1");
		ok &= Expect(context.load_calls == 1, "write failure loads once");
		ok &= Expect(context.capture_calls == 1, "write failure captures once");
		ok &= Expect(context.write_calls == 1, "write failure calls writer once");
		ok &= Expect(context.write_frame_count == 4, "write failure sends four frames to writer");
		ok &= Expect(error.str().contains("Failed to write snapshot"),
		             "write failure writes snapshot error");
		return ok;
	}

	auto MissingDependencyCallbacksStopBeforeCallbacks() -> bool {
		bool ok = true;
		{
			auto context                     = MakeSuccessContext();
			auto dependencies                = TestDependencies(context);
			dependencies.load_runtime_config = nullptr;

			const int result = RunSnapshotWithDependencies(dependencies, {});

			ok &= Expect(result == 1, "missing load dependency returns 1");
			ok &= Expect(context.load_calls == 0, "missing load dependency skips load");
			ok &= Expect(context.capture_calls == 0, "missing load dependency skips capture");
			ok &= Expect(context.write_calls == 0, "missing load dependency skips write");
		}
		{
			auto context                = MakeSuccessContext();
			auto dependencies           = TestDependencies(context);
			dependencies.capture_frames = nullptr;

			const int result = RunSnapshotWithDependencies(dependencies, {});

			ok &= Expect(result == 1, "missing capture dependency returns 1");
			ok &= Expect(context.load_calls == 0, "missing capture dependency skips load");
			ok &= Expect(context.capture_calls == 0, "missing capture dependency skips capture");
			ok &= Expect(context.write_calls == 0, "missing capture dependency skips write");
		}
		{
			auto context                = MakeSuccessContext();
			auto dependencies           = TestDependencies(context);
			dependencies.write_snapshot = nullptr;

			const int result = RunSnapshotWithDependencies(dependencies, {});

			ok &= Expect(result == 1, "missing write dependency returns 1");
			ok &= Expect(context.load_calls == 0, "missing write dependency skips load");
			ok &= Expect(context.capture_calls == 0, "missing write dependency skips capture");
			ok &= Expect(context.write_calls == 0, "missing write dependency skips write");
		}
		return ok;
	}

	auto SuccessPrintsGeneratedPath() -> bool {
		auto               context = MakeSuccessContext();
		std::ostringstream output;
		StreamRedirect     output_redirect(std::cout, output.rdbuf());

		const int result = RunSnapshot(context, {});

		const auto output_text = output.str();
		bool       ok          = true;
		ok &= Expect(result == 0, "successful snapshot returns 0");
		ok &= Expect(context.load_calls == 1, "successful snapshot loads once");
		ok &= Expect(context.capture_calls == 1, "successful snapshot captures once");
		ok &= Expect(context.write_calls == 1, "successful snapshot writes once");
		ok &= Expect(context.capture_config.video.dark_threshold == 41.0F,
		             "capture receives runtime config");
		ok &= Expect(context.write_frame_count == 4, "writer receives four frames");
		ok &= Expect(context.write_config.video.dark_threshold == 41.0F,
		             "writer receives video runtime config");
		ok &= Expect(context.write_config.face.sface_threshold == 0.42F,
		             "writer receives face runtime config");
		ok &=
		    Expect(output_text.contains("Snapshot saved to"), "successful snapshot prints heading");
		ok &= Expect(output_text.contains("/tmp/howdy-test/snapshots/test.jpg"),
		             "successful snapshot prints fixed path");
		return ok;
	}

}  // namespace

auto main() -> int {
	bool ok = true;
	ok &= InvalidRuntimeConfigStopsBeforeCaptureAndWrite();
	ok &= CameraOpenFailureStopsBeforeWrite();
	ok &= CameraReadFailureStopsBeforeWrite();
	ok &= ZeroFrameSuccessfulCaptureStopsBeforeWrite();
	ok &= ShortSuccessfulCaptureStopsBeforeWrite();
	ok &= OversizedSuccessfulCaptureStopsBeforeWrite();
	ok &= UnknownCaptureStatusFailsClosed();
	ok &= WriteFailureReturnsError();
	ok &= EmptyWritePathReturnsError();
	ok &= MissingDependencyCallbacksStopBeforeCallbacks();
	ok &= SuccessPrintsGeneratedPath();
	return ok ? 0 : 1;
}
