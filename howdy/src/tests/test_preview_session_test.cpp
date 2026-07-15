#include "cli/test_cli_internal.hpp"
#include "cli/test_preview_renderer.hpp"
#include "cli/test_preview_session.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <opencv2/imgproc.hpp>

namespace {

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

	auto expect(bool condition, const std::string &message) -> bool {
		if (!condition) {
			std::cerr << "FAIL: " << message << "\n";
		}
		return condition;
	}

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

	void throw_presenter_failure(void *context) {
		(void)context;
		throw std::runtime_error("presenter failed");
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
		howdy::native::VideoConfig config;
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
		howdy::native::VideoConfig config;
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

	auto renderer_owns_configuration_and_models() -> bool {
		auto make_renderer = [] -> test_cli_internal::TestPreviewRenderer {
			howdy::native::VideoConfig config;
			config.clahe_enabled = false;
			std::vector<howdy::native::EncodingModelInfo> models;
			return {config, std::move(models)};
		};

		auto renderer = make_renderer();
		return expect(!renderer.slow_mode(),
		              "renderer remains valid after constructor inputs leave scope");
	}

	struct RendererLifecycleContext {
		std::vector<std::string> sequence;
		int                      initialize_calls     = 0;
		int                      clear_callback_calls = 0;
		int                      destroy_window_calls = 0;
		bool                     fail_initialize      = false;
		bool                     fail_clear_callback  = false;
		bool                     fail_destroy_window  = false;
	};

	void initialize_renderer(test_cli_internal::PreviewRendererInitialization initialization) {
		(void)initialization.renderer;
		auto *lifecycle_context = static_cast<RendererLifecycleContext *>(initialization.context);
		lifecycle_context->sequence.emplace_back("initialize");
		lifecycle_context->initialize_calls++;
		if (lifecycle_context->fail_initialize) {
			throw std::runtime_error("renderer initialization failed");
		}
	}

	void clear_renderer_callback(void *context) {
		auto *lifecycle_context = static_cast<RendererLifecycleContext *>(context);
		lifecycle_context->sequence.emplace_back("clear_callback");
		lifecycle_context->clear_callback_calls++;
		if (lifecycle_context->fail_clear_callback) {
			throw std::runtime_error("renderer callback cleanup failed");
		}
	}

	void destroy_renderer_window(void *context) {
		auto *lifecycle_context = static_cast<RendererLifecycleContext *>(context);
		lifecycle_context->sequence.emplace_back("destroy_window");
		lifecycle_context->destroy_window_calls++;
		if (lifecycle_context->fail_destroy_window) {
			throw std::runtime_error("renderer window cleanup failed");
		}
	}

	auto repeated_preview_replacement_cleans_initialized_renderer_first() -> bool {
		RendererLifecycleContext                                 context;
		howdy::native::VideoConfig                               config;
		const test_cli_internal::TestPreviewRendererDependencies dependencies{
		    .context        = &context,
		    .initialize     = initialize_renderer,
		    .clear_callback = clear_renderer_callback,
		    .destroy_window = destroy_renderer_window,
		};
		std::optional<test_cli_internal::TestPreviewRenderer> renderer;
		test_cli_internal::TestPreviewRendererCleanup         cleanup(renderer);
		renderer.emplace(config, std::vector<howdy::native::EncodingModelInfo>{}, dependencies);
		renderer->initialize();
		context.sequence.clear();

		test_cli_internal::replace_test_preview_renderer(
		    renderer, config, std::vector<howdy::native::EncodingModelInfo>{}, dependencies);
		renderer->initialize();

		bool ok = true;
		ok &= expect(context.sequence ==
		                 std::vector<std::string>{"clear_callback", "destroy_window", "initialize"},
		             "repeat preview cleans old renderer before replacement initialization");
		ok &= expect(context.clear_callback_calls == 1,
		             "repeat preview clears old renderer callback once");
		ok &= expect(context.destroy_window_calls == 1,
		             "repeat preview destroys old renderer window once");
		return ok;
	}

	auto failed_renderer_cleanup_prevents_replacement() -> bool {
		RendererLifecycleContext context;
		context.fail_clear_callback = true;
		context.fail_destroy_window = true;
		howdy::native::VideoConfig                               config;
		const test_cli_internal::TestPreviewRendererDependencies dependencies{
		    .context        = &context,
		    .initialize     = initialize_renderer,
		    .clear_callback = clear_renderer_callback,
		    .destroy_window = destroy_renderer_window,
		};
		std::optional<test_cli_internal::TestPreviewRenderer> renderer;
		test_cli_internal::TestPreviewRendererCleanup         cleanup(renderer);
		renderer.emplace(config, std::vector<howdy::native::EncodingModelInfo>{}, dependencies);
		renderer->initialize();

		std::string error_message;
		try {
			test_cli_internal::replace_test_preview_renderer(
			    renderer, config, std::vector<howdy::native::EncodingModelInfo>{}, dependencies);
		} catch (const std::runtime_error &error) {
			error_message = error.what();
		}
		renderer->initialize();

		bool ok = true;
		ok &= expect(error_message == "renderer callback cleanup failed",
		             "replacement propagates incomplete renderer cleanup");
		ok &= expect(context.initialize_calls == 1,
		             "failed cleanup preserves initialized renderer without rebinding");
		ok &= expect(context.clear_callback_calls == 1,
		             "failed replacement attempts callback cleanup once");
		ok &= expect(context.destroy_window_calls == 1,
		             "failed replacement attempts window cleanup once");
		return ok;
	}

	auto callback_success_window_failure_retries_only_window() -> bool {
		RendererLifecycleContext context;
		context.fail_destroy_window = true;
		howdy::native::VideoConfig                               config;
		const test_cli_internal::TestPreviewRendererDependencies dependencies{
		    .context        = &context,
		    .initialize     = initialize_renderer,
		    .clear_callback = clear_renderer_callback,
		    .destroy_window = destroy_renderer_window,
		};
		std::optional<test_cli_internal::TestPreviewRenderer> renderer;
		test_cli_internal::TestPreviewRendererCleanup         cleanup(renderer);
		renderer.emplace(config, std::vector<howdy::native::EncodingModelInfo>{}, dependencies);
		renderer->initialize();

		bool blocked = false;
		try {
			test_cli_internal::replace_test_preview_renderer(
			    renderer, config, std::vector<howdy::native::EncodingModelInfo>{}, dependencies);
		} catch (const std::runtime_error &) {
			blocked = true;
		}
		renderer->initialize();
		context.fail_destroy_window = false;
		test_cli_internal::replace_test_preview_renderer(
		    renderer, config, std::vector<howdy::native::EncodingModelInfo>{}, dependencies);
		renderer->initialize();

		bool ok = true;
		ok &= expect(blocked, "window cleanup failure blocks renderer replacement");
		ok &=
		    expect(context.clear_callback_calls == 1, "successful callback cleanup is not retried");
		ok &= expect(context.destroy_window_calls == 2, "failed window cleanup is retried once");
		ok &= expect(context.initialize_calls == 2,
		             "replacement waits for window cleanup completion");
		return ok;
	}

	auto callback_failure_window_success_retries_only_callback() -> bool {
		RendererLifecycleContext context;
		context.fail_clear_callback = true;
		howdy::native::VideoConfig                               config;
		const test_cli_internal::TestPreviewRendererDependencies dependencies{
		    .context        = &context,
		    .initialize     = initialize_renderer,
		    .clear_callback = clear_renderer_callback,
		    .destroy_window = destroy_renderer_window,
		};
		std::optional<test_cli_internal::TestPreviewRenderer> renderer;
		test_cli_internal::TestPreviewRendererCleanup         cleanup(renderer);
		renderer.emplace(config, std::vector<howdy::native::EncodingModelInfo>{}, dependencies);
		renderer->initialize();

		bool blocked = false;
		try {
			test_cli_internal::replace_test_preview_renderer(
			    renderer, config, std::vector<howdy::native::EncodingModelInfo>{}, dependencies);
		} catch (const std::runtime_error &) {
			blocked = true;
		}
		renderer->initialize();
		context.fail_clear_callback = false;
		test_cli_internal::replace_test_preview_renderer(
		    renderer, config, std::vector<howdy::native::EncodingModelInfo>{}, dependencies);
		renderer->initialize();

		bool ok = true;
		ok &= expect(blocked, "callback cleanup failure blocks renderer replacement");
		ok &= expect(context.clear_callback_calls == 2, "failed callback cleanup is retried once");
		ok &= expect(context.destroy_window_calls == 1, "successful window cleanup is not retried");
		ok &= expect(context.initialize_calls == 2,
		             "replacement waits for callback cleanup completion");
		return ok;
	}

	auto renderer_default_cleanup_is_ordered_and_idempotent() -> bool {
		RendererLifecycleContext               context;
		howdy::native::VideoConfig             config;
		test_cli_internal::TestPreviewRenderer renderer(
		    config, {},
		    {.context        = &context,
		     .initialize     = initialize_renderer,
		     .clear_callback = clear_renderer_callback,
		     .destroy_window = destroy_renderer_window});

		renderer.shutdown();
		renderer.initialize();
		renderer.initialize();
		renderer.shutdown();
		bool ok = true;
		ok &= expect(context.initialize_calls == 1, "renderer initializes once");
		ok &= expect(context.clear_callback_calls == 1, "renderer clears callback once");
		ok &= expect(context.destroy_window_calls == 1, "renderer destroys window once");
		ok &= expect(context.sequence ==
		                 std::vector<std::string>{"initialize", "clear_callback", "destroy_window"},
		             "renderer clears callback before destroying window");
		return ok;
	}

	auto renderer_failed_initialize_rolls_back() -> bool {
		RendererLifecycleContext context;
		context.fail_initialize = true;
		howdy::native::VideoConfig             config;
		test_cli_internal::TestPreviewRenderer renderer(
		    config, {},
		    {.context        = &context,
		     .initialize     = initialize_renderer,
		     .clear_callback = clear_renderer_callback,
		     .destroy_window = destroy_renderer_window});

		bool threw = false;
		try {
			renderer.initialize();
		} catch (const std::runtime_error &) {
			threw = true;
		}
		bool ok = true;
		ok &= expect(threw, "renderer propagates initialization failure");
		ok &= expect(context.initialize_calls == 1, "renderer attempts initialization once");
		ok &= expect(context.clear_callback_calls == 1,
		             "failed renderer initialization clears callback once");
		ok &= expect(context.destroy_window_calls == 1,
		             "failed renderer initialization destroys window once");
		ok &= expect(context.sequence ==
		                 std::vector<std::string>{"initialize", "clear_callback", "destroy_window"},
		             "failed initialization runs full cleanup in order");
		return ok;
	}

	auto renderer_shutdown_finishes_after_callback_error() -> bool {
		RendererLifecycleContext context;
		context.fail_clear_callback = true;
		howdy::native::VideoConfig             config;
		test_cli_internal::TestPreviewRenderer renderer(
		    config, {},
		    {.context        = &context,
		     .initialize     = initialize_renderer,
		     .clear_callback = clear_renderer_callback,
		     .destroy_window = destroy_renderer_window});
		renderer.initialize();

		bool threw       = false;
		bool retry_threw = false;
		try {
			renderer.shutdown();
		} catch (const std::runtime_error &) {
			threw = true;
		}
		try {
			renderer.shutdown();
		} catch (const std::runtime_error &) {
			retry_threw = true;
		}

		bool ok = true;
		ok &= expect(threw, "renderer propagates callback cleanup failure");
		ok &= expect(retry_threw, "renderer keeps retryable callback cleanup failure");
		ok &= expect(context.clear_callback_calls == 2, "renderer retries failed callback cleanup");
		ok &= expect(context.destroy_window_calls == 1,
		             "renderer destroys window after callback cleanup failure");
		return ok;
	}

	auto renderer_partial_initialization_preserves_primary_failure() -> bool {
		RendererLifecycleContext context;
		context.fail_initialize     = true;
		context.fail_clear_callback = true;
		context.fail_destroy_window = true;
		howdy::native::VideoConfig             config;
		test_cli_internal::TestPreviewRenderer renderer(
		    config, {},
		    {.context        = &context,
		     .initialize     = initialize_renderer,
		     .clear_callback = clear_renderer_callback,
		     .destroy_window = destroy_renderer_window});

		std::string error_message;
		try {
			renderer.initialize();
		} catch (const std::runtime_error &error) {
			error_message = error.what();
		}

		bool ok = true;
		ok &= expect(error_message == "renderer initialization failed",
		             "cleanup failures do not mask initialization failure");
		ok &= expect(context.clear_callback_calls == 1,
		             "partial initialization attempts callback cleanup once");
		ok &= expect(context.destroy_window_calls == 1,
		             "partial initialization attempts window cleanup once");
		return ok;
	}

	auto renderer_already_closed_window_cleanup_is_idempotent() -> bool {
		RendererLifecycleContext context;
		context.fail_clear_callback = true;
		context.fail_destroy_window = true;
		howdy::native::VideoConfig             config;
		test_cli_internal::TestPreviewRenderer renderer(
		    config, {},
		    {.context        = &context,
		     .initialize     = initialize_renderer,
		     .clear_callback = clear_renderer_callback,
		     .destroy_window = destroy_renderer_window});
		renderer.initialize();

		std::string error_message;
		bool        retry_threw = false;
		try {
			renderer.shutdown();
		} catch (const std::runtime_error &error) {
			error_message = error.what();
		}
		try {
			renderer.shutdown();
		} catch (const std::runtime_error &) {
			retry_threw = true;
		}

		bool ok = true;
		ok &= expect(error_message == "renderer callback cleanup failed",
		             "shutdown preserves first cleanup failure");
		ok &= expect(retry_threw, "shutdown keeps retryable cleanup failures");
		ok &= expect(context.clear_callback_calls == 2,
		             "incomplete callback cleanup remains retryable");
		ok &= expect(context.destroy_window_calls == 2,
		             "incomplete window cleanup remains retryable");
		return ok;
	}

	auto presenter_failure_survives_real_owner_cleanup_failures() -> bool {
		RendererLifecycleContext renderer_context;
		renderer_context.fail_clear_callback = true;
		renderer_context.fail_destroy_window = true;
		howdy::native::VideoConfig config;

		std::string error_message;
		try {
			std::optional<test_cli_internal::TestPreviewRenderer> renderer;
			renderer.emplace(config, std::vector<howdy::native::EncodingModelInfo>{},
			                 test_cli_internal::TestPreviewRendererDependencies{
			                     .context        = &renderer_context,
			                     .initialize     = initialize_renderer,
			                     .clear_callback = clear_renderer_callback,
			                     .destroy_window = destroy_renderer_window,
			                 });
			renderer->initialize();
			test_cli_internal::run_with_preview_cleanup(renderer, nullptr, throw_presenter_failure);
		} catch (const std::runtime_error &error) {
			error_message = error.what();
		}

		bool ok = true;
		ok &= expect(error_message == "presenter failed",
		             "renderer cleanup failures do not mask presenter failure");
		ok &= expect(renderer_context.clear_callback_calls == 1,
		             "presenter failure attempts callback cleanup");
		ok &= expect(renderer_context.destroy_window_calls == 1,
		             "presenter failure attempts window cleanup");
		return ok;
	}

	auto disabled_exposure_skips_restore() -> bool {
		SessionContext             context;
		howdy::native::VideoConfig config;
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
		howdy::native::VideoConfig config;
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
		howdy::native::VideoConfig config;
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
		howdy::native::VideoConfig config;
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
		howdy::native::VideoConfig config;
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

auto main() -> int {
	bool ok = true;
	ok &= first_frame_quit_restores_configured_exposure();
	ok &= retained_production_frame_supports_two_preview_runs();
	ok &= renderer_owns_configuration_and_models();
	ok &= repeated_preview_replacement_cleans_initialized_renderer_first();
	ok &= failed_renderer_cleanup_prevents_replacement();
	ok &= callback_success_window_failure_retries_only_window();
	ok &= callback_failure_window_success_retries_only_callback();
	ok &= renderer_default_cleanup_is_ordered_and_idempotent();
	ok &= renderer_failed_initialize_rolls_back();
	ok &= renderer_shutdown_finishes_after_callback_error();
	ok &= renderer_partial_initialization_preserves_primary_failure();
	ok &= renderer_already_closed_window_cleanup_is_idempotent();
	ok &= presenter_failure_survives_real_owner_cleanup_failures();
	ok &= disabled_exposure_skips_restore();
	ok &= camera_read_failure_after_prefetch_maps_to_camera_error();
	ok &= slow_mode_sleeps_and_restores_exposure_between_frames();
	ok &= inference_failure_stops_before_presenter();
	ok &= missing_dependency_fails_closed();
	return ok ? 0 : 1;
}
