#include "cli/test/internal.hpp"
#include "cli/test/preview_session.hpp"
#include "test_support.hpp"

#include <chrono>
#include <stdexcept>
#include <vector>

#include <opencv2/imgproc.hpp>

namespace {

	using howdy::test::expect;

	namespace test_cli_internal = howdy::native::test_cli_internal;

	struct SessionContext {
		std::vector<cv::Mat>                                  frames;
		std::vector<test_cli_internal::TestPreviewFrameStats> presented_stats;
		std::size_t                                           next_frame            = 0;
		int                                                   read_calls            = 0;
		int                                                   present_calls         = 0;
		int                                                   restore_calls         = 0;
		int                                                   restore_calls_at_read = -1;
		int                                                   sleep_calls           = 0;
		std::chrono::milliseconds                             slept_for{0};
		bool                                                  slow_mode       = false;
		int                                                   stop_after      = 1;
		bool                                                  invalid_prepare = false;
		bool                                                  throw_present   = false;
		std::chrono::steady_clock::time_point                 now;
	};

	auto prepare_frame(void *context, const cv::Mat &gray_frame) -> cv::Mat {
		auto *session_context = static_cast<SessionContext *>(context);
		if (session_context->invalid_prepare) {
			return {};
		}
		cv::Mat prepared;
		cv::cvtColor(gray_frame, prepared, cv::COLOR_GRAY2BGR);
		return prepared;
	}

	auto detect_faces(void *context, const cv::Mat &frame) -> howdy::native::FaceDetectionResult {
		(void)context;
		(void)frame;
		return {};
	}

	auto engine_now(void *context) -> std::chrono::steady_clock::time_point {
		return static_cast<SessionContext *>(context)->now;
	}

	auto read_gray_frame(void *context, cv::Mat &gray_frame) -> bool {
		auto *session_context = static_cast<SessionContext *>(context);
		session_context->read_calls++;
		session_context->restore_calls_at_read = session_context->restore_calls;
		if (session_context->next_frame >= session_context->frames.size()) {
			return false;
		}
		gray_frame = session_context->frames[session_context->next_frame++].clone();
		return true;
	}

	void restore_exposure(void *context) {
		static_cast<SessionContext *>(context)->restore_calls++;
	}

	auto present_frame(void *context, const howdy::native::PreviewFrameResult &frame_result,
	                   const test_cli_internal::TestPreviewFrameStats &stats) -> bool {
		(void)frame_result;
		auto *session_context = static_cast<SessionContext *>(context);
		session_context->present_calls++;
		session_context->presented_stats.push_back(stats);
		if (session_context->throw_present) {
			throw std::runtime_error("presenter failed");
		}
		return session_context->present_calls < session_context->stop_after;
	}

	auto slow_mode(void *context) -> bool {
		return static_cast<SessionContext *>(context)->slow_mode;
	}

	auto session_now(void *context) -> std::chrono::steady_clock::time_point {
		return static_cast<SessionContext *>(context)->now;
	}

	void sleep_for(void *context, std::chrono::milliseconds duration) {
		auto *session_context = static_cast<SessionContext *>(context);
		session_context->sleep_calls++;
		session_context->slept_for += duration;
	}

	auto make_engine(SessionContext &context, const howdy::native::VideoConfig &config)
	    -> howdy::native::PreviewEngine {
		return {
		    config,
		    {
		        .context       = &context,
		        .prepare_frame = prepare_frame,
		        .detect_faces  = detect_faces,
		        .now           = engine_now,
		    },
		    {},
		    0,
		    false,
		};
	}

	auto make_dependencies(SessionContext &context)
	    -> test_cli_internal::TestPreviewSessionDependencies {
		return {
		    .capture_context  = &context,
		    .read_gray_frame  = read_gray_frame,
		    .restore_exposure = restore_exposure,
		    .renderer_context = &context,
		    .present          = present_frame,
		    .slow_mode        = slow_mode,
		    .clock_context    = &context,
		    .now              = session_now,
		    .sleep_context    = &context,
		    .sleep            = sleep_for,
		};
	}

	auto gray_frame() -> cv::Mat {
		return cv::Mat(8, 8, CV_8UC1, cv::Scalar(128)).clone();
	}

	auto first_frame_quit_restores_configured_exposure() -> bool {
		SessionContext             context;
		howdy::native::VideoConfig config{};
		config.clahe_enabled                         = false;
		config.dark_threshold                        = 100.0F;
		config.exposure                              = 17;
		auto                                  engine = make_engine(context, config);
		test_cli_internal::TestPreviewSession session(config, engine, make_dependencies(context));

		const auto result = session.run(gray_frame());

		bool ok = true;
		ok &= expect(result.status == test_cli_internal::TestPreviewStatus::kOk,
		             "presenter exit returns success");
		ok &= expect(context.read_calls == 0, "prefetched frame skips camera read");
		ok &= expect(context.present_calls == 1, "prefetched frame presents once");
		ok &= expect(context.presented_stats.size() == 1,
		             "prefetched frame records one presentation stat");
		ok &= expect(context.presented_stats.front().total_frames == 1,
		             "prefetched frame starts frame count at one");
		ok &= expect(context.sleep_calls == 0, "presenter exit skips slow mode sleep");
		ok &= expect(context.restore_calls == 1,
		             "first-frame presenter exit restores configured exposure");
		return ok;
	}

	auto retained_production_frame_supports_two_preview_runs() -> bool {
		howdy::native::VideoConfig config{};
		config.clahe_enabled                                = false;
		config.dark_threshold                               = 100.0F;
		auto                                  preview_frame = gray_frame();
		SessionContext                        first_context;
		auto                                  first_engine = make_engine(first_context, config);
		test_cli_internal::TestPreviewSession first_session(config, first_engine,
		                                                    make_dependencies(first_context));
		SessionContext                        second_context;
		auto                                  second_engine = make_engine(second_context, config);
		test_cli_internal::TestPreviewSession second_session(config, second_engine,
		                                                     make_dependencies(second_context));

		const auto first_result = test_cli_internal::run_preview_session_with_retained_frame(
		    first_session, preview_frame);
		const auto second_result = test_cli_internal::run_preview_session_with_retained_frame(
		    second_session, preview_frame);

		bool ok = true;
		ok &= expect(first_result.status == test_cli_internal::TestPreviewStatus::kOk,
		             "first retained prefetched frame preview returns success");
		ok &= expect(second_result.status == test_cli_internal::TestPreviewStatus::kOk,
		             "second retained prefetched frame preview returns success");
		ok &= expect(first_context.present_calls == 1 && second_context.present_calls == 1,
		             "both sessions present caller-owned prefetched frame");
		ok &= expect(first_context.read_calls == 0 && second_context.read_calls == 0,
		             "both sessions skip capture read for prefetched frame");
		ok &=
		    expect(!preview_frame.empty(), "both sessions preserve caller-owned prefetched frame");
		return ok;
	}

	auto disabled_exposure_skips_restore() -> bool {
		SessionContext             context;
		howdy::native::VideoConfig config{};
		config.clahe_enabled                         = false;
		config.dark_threshold                        = 100.0F;
		config.exposure                              = -1;
		auto                                  engine = make_engine(context, config);
		test_cli_internal::TestPreviewSession session(config, engine, make_dependencies(context));

		const auto result = session.run(gray_frame());

		bool ok = true;
		ok &= expect(result.status == test_cli_internal::TestPreviewStatus::kOk,
		             "disabled exposure presenter exit returns success");
		ok &= expect(context.restore_calls == 0, "disabled exposure skips restore");
		return ok;
	}

	auto camera_read_failure_after_prefetch_maps_to_camera_error() -> bool {
		SessionContext context;
		context.stop_after = 2;
		howdy::native::VideoConfig config{};
		config.clahe_enabled                         = false;
		config.dark_threshold                        = 100.0F;
		config.exposure                              = 17;
		auto                                  engine = make_engine(context, config);
		test_cli_internal::TestPreviewSession session(config, engine, make_dependencies(context));

		const auto result = session.run(gray_frame());

		bool ok = true;
		ok &= expect(result.status == test_cli_internal::TestPreviewStatus::kCameraReadError,
		             "read failure after prefetch returns camera error");
		ok &= expect(context.present_calls == 1, "read failure presents prefetched frame once");
		ok &= expect(context.read_calls == 1, "read failure attempts one subsequent camera read");
		ok &= expect(context.restore_calls_at_read == 1,
		             "completed prefetched frame restores exposure once");
		ok &= expect(context.restore_calls == 2,
		             "camera-read failure restores exposure exactly once");
		return ok;
	}

	auto slow_mode_sleeps_and_restores_exposure_between_frames() -> bool {
		SessionContext context;
		context.frames.push_back(gray_frame());
		context.slow_mode  = true;
		context.stop_after = 2;
		howdy::native::VideoConfig config{};
		config.clahe_enabled                         = false;
		config.dark_threshold                        = 100.0F;
		config.exposure                              = 17;
		auto                                  engine = make_engine(context, config);
		test_cli_internal::TestPreviewSession session(config, engine, make_dependencies(context));

		const auto result = session.run(gray_frame());

		bool ok = true;
		ok &= expect(result.status == test_cli_internal::TestPreviewStatus::kOk,
		             "second presenter exit returns success");
		ok &= expect(context.present_calls == 2, "slow mode presents two frames");
		ok &= expect(context.read_calls == 1, "slow mode reads second frame once");
		ok &= expect(context.sleep_calls == 1, "slow mode sleeps between frames");
		ok &= expect(context.slept_for == std::chrono::milliseconds(500),
		             "slow mode fills 500ms frame interval");
		ok &= expect(context.restore_calls == 2, "exposure restores at both frame boundaries");
		return ok;
	}

	auto inference_failure_stops_before_presenter() -> bool {
		SessionContext context;
		context.invalid_prepare = true;
		howdy::native::VideoConfig config{};
		config.clahe_enabled                         = false;
		config.dark_threshold                        = 100.0F;
		config.exposure                              = 17;
		auto                                  engine = make_engine(context, config);
		test_cli_internal::TestPreviewSession session(config, engine, make_dependencies(context));

		const auto result = session.run(gray_frame());

		bool ok = true;
		ok &= expect(result.status == test_cli_internal::TestPreviewStatus::kFaceModelError,
		             "inference failure returns face model error");
		ok &= expect(result.error_message.contains("Prepared frame"),
		             "inference failure preserves engine error");
		ok &= expect(context.present_calls == 0, "inference failure skips presenter");
		ok &=
		    expect(context.restore_calls == 1, "inference failure restores exposure exactly once");
		return ok;
	}

	auto missing_dependency_fails_closed() -> bool {
		SessionContext             context;
		howdy::native::VideoConfig config{};
		config.clahe_enabled  = false;
		config.dark_threshold = 100.0F;
		auto engine           = make_engine(context, config);
		auto dependencies     = make_dependencies(context);
		dependencies.present  = nullptr;
		test_cli_internal::TestPreviewSession session(config, engine, dependencies);

		const auto result = session.run(gray_frame());

		bool ok = true;
		ok &= expect(result.status == test_cli_internal::TestPreviewStatus::kFaceModelError,
		             "missing session dependency returns face model error");
		ok &= expect(result.error_message.contains("missing test preview session dependency"),
		             "missing session dependency writes internal error");
		ok &= expect(context.present_calls == 0, "missing dependency skips preview work");
		return ok;
	}

}  // namespace

auto run_test_preview_renderer_tests() -> bool;

auto main() -> int {
	bool ok = true;
	ok &= first_frame_quit_restores_configured_exposure();
	ok &= retained_production_frame_supports_two_preview_runs();
	ok &= run_test_preview_renderer_tests();
	ok &= disabled_exposure_skips_restore();
	ok &= camera_read_failure_after_prefetch_maps_to_camera_error();
	ok &= slow_mode_sleeps_and_restores_exposure_between_frames();
	ok &= inference_failure_stops_before_presenter();
	ok &= missing_dependency_fails_closed();
	return ok ? 0 : 1;
}
