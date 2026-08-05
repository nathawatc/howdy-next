#include "cli/snapshot_internal.hpp"
#include "test_support.hpp"

#include <bit>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <opencv2/core.hpp>

namespace {

	using howdy::test::expect;

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

	auto valid_config_load_result() -> howdy::native::RuntimeConfigLoadResult {
		howdy::native::RuntimeConfig config;
		config.video.dark_threshold = 41.0F;
		config.face.sface_threshold = 0.42F;
		return howdy::native::RuntimeConfigLoadResult{
		    .ok     = true,
		    .status = howdy::native::RuntimeConfigLoadStatus::kOk,
		    .config = config,
		};
	}

	auto invalid_config_load_result() -> howdy::native::RuntimeConfigLoadResult {
		return howdy::native::RuntimeConfigLoadResult{
		    .ok            = false,
		    .status        = howdy::native::RuntimeConfigLoadStatus::kInvalidRuntimeValue,
		    .error_message = "invalid runtime config",
		};
	}

	auto dummy_frames() -> std::vector<cv::Mat> {
		return {
		    cv::Mat(1, 1, CV_8UC3, cv::Scalar(1, 2, 3)),
		    cv::Mat(1, 1, CV_8UC3, cv::Scalar(4, 5, 6)),
		    cv::Mat(1, 1, CV_8UC3, cv::Scalar(7, 8, 9)),
		    cv::Mat(1, 1, CV_8UC3, cv::Scalar(10, 11, 12)),
		};
	}

	auto successful_capture_result() -> howdy::native::snapshot_internal::SnapshotCaptureResult {
		return howdy::native::snapshot_internal::SnapshotCaptureResult{
		    .status = howdy::native::snapshot_internal::SnapshotCaptureStatus::kOk,
		    .frames = dummy_frames(),
		};
	}

	auto load_runtime_config_callback(void *raw_context) -> howdy::native::RuntimeConfigLoadResult {
		auto *context = static_cast<SnapshotCliTestContext *>(raw_context);
		++context->load_calls;
		return context->config_result;
	}

	auto capture_frames_callback(void *raw_context, const howdy::native::RuntimeConfig &config)
	    -> howdy::native::snapshot_internal::SnapshotCaptureResult {
		auto *context = static_cast<SnapshotCliTestContext *>(raw_context);
		++context->capture_calls;
		context->capture_config = config;
		return context->capture_result;
	}

	auto write_snapshot_callback(void *raw_context, const std::vector<cv::Mat> &frames,
	                             const howdy::native::RuntimeConfig &config)
	    -> howdy::native::snapshot_internal::SnapshotWriteResult {
		auto *context = static_cast<SnapshotCliTestContext *>(raw_context);
		++context->write_calls;
		context->write_frame_count = frames.size();
		context->write_config      = config;
		return context->write_result;
	}

	auto test_dependencies(SnapshotCliTestContext &context)
	    -> howdy::native::snapshot_internal::SnapshotDependencies {
		return howdy::native::snapshot_internal::SnapshotDependencies{
		    .context             = &context,
		    .load_runtime_config = load_runtime_config_callback,
		    .capture_frames      = capture_frames_callback,
		    .write_snapshot      = write_snapshot_callback,
		};
	}

	auto run_snapshot_with_dependencies(
	    howdy::native::snapshot_internal::SnapshotDependencies dependencies,
	    std::vector<std::string>                               arguments) -> int {
		std::vector<char *> argv;
		argv.reserve(arguments.size());
		for (auto &argument : arguments) {
			argv.push_back(argument.data());
		}
		return howdy::native::snapshot_internal::snapshot_main_with_dependencies(
		    static_cast<int>(argv.size()), argv.data(), dependencies);
	}

	auto run_snapshot(SnapshotCliTestContext &context, std::vector<std::string> arguments) -> int {
		return run_snapshot_with_dependencies(test_dependencies(context), std::move(arguments));
	}

	auto make_success_context() -> SnapshotCliTestContext {
		SnapshotCliTestContext context;
		context.config_result  = valid_config_load_result();
		context.capture_result = successful_capture_result();
		context.write_result   = howdy::native::snapshot_internal::SnapshotWriteResult{
		    .ok   = true,
		    .path = "/tmp/howdy-test/snapshots/test.jpg",
		};
		return context;
	}

	auto invalid_runtime_config_stops_before_capture_and_write() -> bool {
		auto context          = make_success_context();
		context.config_result = invalid_config_load_result();
		std::ostringstream error;
		StreamRedirect     error_redirect(std::cerr, error.rdbuf());

		const int result = run_snapshot(context, {"howdy-snapshot"});

		bool ok = true;
		ok &= expect(result == 1, "invalid runtime config returns 1");
		ok &= expect(context.load_calls == 1, "invalid runtime config loads once");
		ok &= expect(context.capture_calls == 0, "invalid runtime config skips capture");
		ok &= expect(context.write_calls == 0, "invalid runtime config skips write");
		ok &= expect(error.str().contains("invalid runtime config"),
		             "invalid runtime config writes config error");
		return ok;
	}

	auto camera_open_failure_stops_before_write() -> bool {
		auto context           = make_success_context();
		context.capture_result = howdy::native::snapshot_internal::SnapshotCaptureResult{
		    .status        = howdy::native::snapshot_internal::SnapshotCaptureStatus::kOpenError,
		    .error_message = "camera open failed",
		};
		std::ostringstream error;
		StreamRedirect     error_redirect(std::cerr, error.rdbuf());

		const int result = run_snapshot(context, {"howdy-snapshot"});

		bool ok = true;
		ok &= expect(result == 1, "camera open failure returns 1");
		ok &= expect(context.load_calls == 1, "camera open failure loads once");
		ok &= expect(context.capture_calls == 1, "camera open failure captures once");
		ok &= expect(context.write_calls == 0, "camera open failure skips write");
		ok &= expect(error.str().contains("camera open failed"),
		             "camera open failure writes injected error");
		return ok;
	}

	auto camera_read_failure_stops_before_write() -> bool {
		auto context           = make_success_context();
		context.capture_result = howdy::native::snapshot_internal::SnapshotCaptureResult{
		    .status = howdy::native::snapshot_internal::SnapshotCaptureStatus::kReadError,
		};
		std::ostringstream error;
		StreamRedirect     error_redirect(std::cerr, error.rdbuf());

		const int result = run_snapshot(context, {"howdy-snapshot"});

		bool ok = true;
		ok &= expect(result == 1, "camera read failure returns 1");
		ok &= expect(context.load_calls == 1, "camera read failure loads once");
		ok &= expect(context.capture_calls == 1, "camera read failure captures once");
		ok &= expect(context.write_calls == 0, "camera read failure skips write");
		ok &= expect(error.str().contains("Could not capture a camera frame"),
		             "camera read failure writes read error");
		return ok;
	}

	auto unexpected_successful_capture_frame_count_stops_before_write(std::size_t frame_count,
	                                                                  const std::string &test_name)
	    -> bool {
		auto context = make_success_context();
		context.capture_result.frames.clear();
		const auto frames = dummy_frames();
		for (std::size_t index = 0; index < frame_count; ++index) {
			context.capture_result.frames.push_back(frames[index % frames.size()]);
		}
		std::ostringstream error;
		StreamRedirect     error_redirect(std::cerr, error.rdbuf());

		const int result = run_snapshot(context, {"howdy-snapshot"});

		bool ok = true;
		ok &= expect(result == 1, test_name + " returns 1");
		ok &= expect(context.load_calls == 1, test_name + " loads once");
		ok &= expect(context.capture_calls == 1, test_name + " captures once");
		ok &= expect(context.write_calls == 0, test_name + " skips write");
		ok &= expect(error.str().contains(
		                 "Internal error: snapshot capture returned unexpected frame count"),
		             test_name + " writes frame count error");
		return ok;
	}

	auto zero_frame_successful_capture_stops_before_write() -> bool {
		return unexpected_successful_capture_frame_count_stops_before_write(
		    0, "zero-frame successful capture");
	}

	auto short_successful_capture_stops_before_write() -> bool {
		return unexpected_successful_capture_frame_count_stops_before_write(
		    kSnapshotFrameCount - 1, "short successful capture");
	}

	auto oversized_successful_capture_stops_before_write() -> bool {
		return unexpected_successful_capture_frame_count_stops_before_write(
		    kSnapshotFrameCount + 1, "oversized successful capture");
	}

	auto unknown_capture_status_fails_closed() -> bool {
		auto context = make_success_context();
		context.capture_result.status =
		    std::bit_cast<howdy::native::snapshot_internal::SnapshotCaptureStatus>(
		        std::uint8_t{UINT8_MAX});
		std::ostringstream error;
		StreamRedirect     error_redirect(std::cerr, error.rdbuf());

		const int result = run_snapshot(context, {"howdy-snapshot"});

		bool ok = true;
		ok &= expect(result == 1, "unknown capture status returns 1");
		ok &= expect(context.load_calls == 1, "unknown capture status loads once");
		ok &= expect(context.capture_calls == 1, "unknown capture status captures once");
		ok &= expect(context.write_calls == 0, "unknown capture status skips write");
		ok &= expect(error.str().contains("Internal error: unknown snapshot capture status"),
		             "unknown capture status writes internal error");
		return ok;
	}

	auto empty_write_path_returns_error() -> bool {
		auto context         = make_success_context();
		context.write_result = howdy::native::snapshot_internal::SnapshotWriteResult{
		    .ok = true,
		};
		std::ostringstream error;
		StreamRedirect     error_redirect(std::cerr, error.rdbuf());

		const int result = run_snapshot(context, {"howdy-snapshot"});

		bool ok = true;
		ok &= expect(result == 1, "empty write path returns 1");
		ok &= expect(context.load_calls == 1, "empty write path loads once");
		ok &= expect(context.capture_calls == 1, "empty write path captures once");
		ok &= expect(context.write_calls == 1, "empty write path calls writer once");
		ok &= expect(error.str().contains("Failed to write snapshot"),
		             "empty write path writes snapshot error");
		return ok;
	}

	auto write_failure_returns_error() -> bool {
		auto context         = make_success_context();
		context.write_result = howdy::native::snapshot_internal::SnapshotWriteResult{};
		std::ostringstream error;
		StreamRedirect     error_redirect(std::cerr, error.rdbuf());

		const int result = run_snapshot(context, {"howdy-snapshot"});

		bool ok = true;
		ok &= expect(result == 1, "write failure returns 1");
		ok &= expect(context.load_calls == 1, "write failure loads once");
		ok &= expect(context.capture_calls == 1, "write failure captures once");
		ok &= expect(context.write_calls == 1, "write failure calls writer once");
		ok &= expect(context.write_frame_count == 4, "write failure sends four frames to writer");
		ok &= expect(error.str().contains("Failed to write snapshot"),
		             "write failure writes snapshot error");
		return ok;
	}

	auto missing_dependency_callbacks_stop_before_callbacks() -> bool {
		bool ok = true;
		{
			auto context                     = make_success_context();
			auto dependencies                = test_dependencies(context);
			dependencies.load_runtime_config = nullptr;

			const int result = run_snapshot_with_dependencies(dependencies, {"howdy-snapshot"});

			ok &= expect(result == 1, "missing load dependency returns 1");
			ok &= expect(context.load_calls == 0, "missing load dependency skips load");
			ok &= expect(context.capture_calls == 0, "missing load dependency skips capture");
			ok &= expect(context.write_calls == 0, "missing load dependency skips write");
		}
		{
			auto context                = make_success_context();
			auto dependencies           = test_dependencies(context);
			dependencies.capture_frames = nullptr;

			const int result = run_snapshot_with_dependencies(dependencies, {"howdy-snapshot"});

			ok &= expect(result == 1, "missing capture dependency returns 1");
			ok &= expect(context.load_calls == 0, "missing capture dependency skips load");
			ok &= expect(context.capture_calls == 0, "missing capture dependency skips capture");
			ok &= expect(context.write_calls == 0, "missing capture dependency skips write");
		}
		{
			auto context                = make_success_context();
			auto dependencies           = test_dependencies(context);
			dependencies.write_snapshot = nullptr;

			const int result = run_snapshot_with_dependencies(dependencies, {"howdy-snapshot"});

			ok &= expect(result == 1, "missing write dependency returns 1");
			ok &= expect(context.load_calls == 0, "missing write dependency skips load");
			ok &= expect(context.capture_calls == 0, "missing write dependency skips capture");
			ok &= expect(context.write_calls == 0, "missing write dependency skips write");
		}
		return ok;
	}

	auto success_prints_generated_path() -> bool {
		auto               context = make_success_context();
		std::ostringstream output;
		StreamRedirect     output_redirect(std::cout, output.rdbuf());

		const int result = run_snapshot(context, {"howdy-snapshot"});

		const auto output_text = output.str();
		bool       ok          = true;
		ok &= expect(result == 0, "successful snapshot returns 0");
		ok &= expect(context.load_calls == 1, "successful snapshot loads once");
		ok &= expect(context.capture_calls == 1, "successful snapshot captures once");
		ok &= expect(context.write_calls == 1, "successful snapshot writes once");
		ok &= expect(context.capture_config.video.dark_threshold == 41.0F,
		             "capture receives runtime config");
		ok &= expect(context.write_frame_count == 4, "writer receives four frames");
		ok &= expect(context.write_config.video.dark_threshold == 41.0F,
		             "writer receives video runtime config");
		ok &= expect(context.write_config.face.sface_threshold == 0.42F,
		             "writer receives face runtime config");
		ok &=
		    expect(output_text.contains("Snapshot saved to"), "successful snapshot prints heading");
		ok &= expect(output_text.contains("/tmp/howdy-test/snapshots/test.jpg"),
		             "successful snapshot prints fixed path");
		return ok;
	}

}  // namespace

auto main() -> int {
	bool ok = true;
	ok &= invalid_runtime_config_stops_before_capture_and_write();
	ok &= camera_open_failure_stops_before_write();
	ok &= camera_read_failure_stops_before_write();
	ok &= zero_frame_successful_capture_stops_before_write();
	ok &= short_successful_capture_stops_before_write();
	ok &= oversized_successful_capture_stops_before_write();
	ok &= unknown_capture_status_fails_closed();
	ok &= write_failure_returns_error();
	ok &= empty_write_path_returns_error();
	ok &= missing_dependency_callbacks_stop_before_callbacks();
	ok &= success_prints_generated_path();
	return ok ? 0 : 1;
}
