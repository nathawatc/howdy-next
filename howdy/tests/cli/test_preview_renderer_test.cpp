#include "cli/test_cli_internal.hpp"
#include "cli/test_preview_renderer.hpp"
#include "test_support.hpp"

#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

	using howdy::test::expect;

	namespace test_cli_internal = howdy::native::test_cli_internal;

	void throw_presenter_failure(void *context) {
		(void)context;
		throw std::runtime_error("presenter failed");
	}

	auto renderer_owns_configuration_and_models() -> bool {
		auto make_renderer = [] -> test_cli_internal::TestPreviewRenderer {
			howdy::native::VideoConfig config{};
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
		howdy::native::VideoConfig                               config{};
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
		howdy::native::VideoConfig                               config{};
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
		howdy::native::VideoConfig                               config{};
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
		howdy::native::VideoConfig                               config{};
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
		howdy::native::VideoConfig             config{};
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
		howdy::native::VideoConfig             config{};
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
		howdy::native::VideoConfig             config{};
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
		howdy::native::VideoConfig             config{};
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
		howdy::native::VideoConfig             config{};
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
		howdy::native::VideoConfig config{};

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

}  // namespace

auto run_test_preview_renderer_tests() -> bool;

auto run_test_preview_renderer_tests() -> bool {
	bool ok = true;
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
	return ok;
}
