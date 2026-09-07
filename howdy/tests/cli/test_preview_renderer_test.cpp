#include "cli/test/internal.hpp"
#include "cli/test/preview_renderer.hpp"
#include "test_support.hpp"

#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

	using howdy::test::expect;

	namespace test_cli_internal = howdy::native::test_cli_internal;

	void ThrowPresenterFailure(void *context) {
		(void)context;
		throw std::runtime_error("presenter failed");
	}

	auto RendererOwnsConfigurationAndModels() -> bool {
		auto make_renderer = [] -> test_cli_internal::TestPreviewRenderer {
			howdy::native::VideoConfig config{};
			config.clahe_enabled = false;
			std::vector<howdy::native::EncodingModelInfo> models;
			return {config, std::move(models)};
		};

		auto renderer = make_renderer();
		return expect(!renderer.SlowMode(),
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

	void InitializeRenderer(test_cli_internal::PreviewRendererInitialization initialization) {
		(void)initialization.renderer;
		auto *lifecycle_context = static_cast<RendererLifecycleContext *>(initialization.context);
		lifecycle_context->sequence.emplace_back("initialize");
		lifecycle_context->initialize_calls++;
		if (lifecycle_context->fail_initialize) {
			throw std::runtime_error("renderer initialization failed");
		}
	}

	void ClearRendererCallback(void *context) {
		auto *lifecycle_context = static_cast<RendererLifecycleContext *>(context);
		lifecycle_context->sequence.emplace_back("clear_callback");
		lifecycle_context->clear_callback_calls++;
		if (lifecycle_context->fail_clear_callback) {
			throw std::runtime_error("renderer callback cleanup failed");
		}
	}

	void DestroyRendererWindow(void *context) {
		auto *lifecycle_context = static_cast<RendererLifecycleContext *>(context);
		lifecycle_context->sequence.emplace_back("destroy_window");
		lifecycle_context->destroy_window_calls++;
		if (lifecycle_context->fail_destroy_window) {
			throw std::runtime_error("renderer window cleanup failed");
		}
	}

	auto RepeatedPreviewReplacementCleansInitializedRendererFirst() -> bool {
		RendererLifecycleContext                                 context;
		howdy::native::VideoConfig                               config{};
		const test_cli_internal::TestPreviewRendererDependencies dependencies{
		    .context        = &context,
		    .initialize     = InitializeRenderer,
		    .clear_callback = ClearRendererCallback,
		    .destroy_window = DestroyRendererWindow,
		};
		std::optional<test_cli_internal::TestPreviewRenderer> renderer;
		test_cli_internal::TestPreviewRendererCleanup         cleanup(renderer);
		renderer.emplace(config, std::vector<howdy::native::EncodingModelInfo>{}, dependencies);
		renderer->Initialize();
		context.sequence.clear();

		test_cli_internal::ReplaceTestPreviewRenderer(
		    renderer, config, std::vector<howdy::native::EncodingModelInfo>{}, dependencies);
		renderer->Initialize();

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

	auto FailedRendererCleanupPreventsReplacement() -> bool {
		RendererLifecycleContext context;
		context.fail_clear_callback = true;
		context.fail_destroy_window = true;
		howdy::native::VideoConfig                               config{};
		const test_cli_internal::TestPreviewRendererDependencies dependencies{
		    .context        = &context,
		    .initialize     = InitializeRenderer,
		    .clear_callback = ClearRendererCallback,
		    .destroy_window = DestroyRendererWindow,
		};
		std::optional<test_cli_internal::TestPreviewRenderer> renderer;
		test_cli_internal::TestPreviewRendererCleanup         cleanup(renderer);
		renderer.emplace(config, std::vector<howdy::native::EncodingModelInfo>{}, dependencies);
		renderer->Initialize();

		std::string error_message;
		try {
			test_cli_internal::ReplaceTestPreviewRenderer(
			    renderer, config, std::vector<howdy::native::EncodingModelInfo>{}, dependencies);
		} catch (const std::runtime_error &error) {
			error_message = error.what();
		}
		renderer->Initialize();

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

	auto CallbackSuccessWindowFailureRetriesOnlyWindow() -> bool {
		RendererLifecycleContext context;
		context.fail_destroy_window = true;
		howdy::native::VideoConfig                               config{};
		const test_cli_internal::TestPreviewRendererDependencies dependencies{
		    .context        = &context,
		    .initialize     = InitializeRenderer,
		    .clear_callback = ClearRendererCallback,
		    .destroy_window = DestroyRendererWindow,
		};
		std::optional<test_cli_internal::TestPreviewRenderer> renderer;
		test_cli_internal::TestPreviewRendererCleanup         cleanup(renderer);
		renderer.emplace(config, std::vector<howdy::native::EncodingModelInfo>{}, dependencies);
		renderer->Initialize();

		bool blocked = false;
		try {
			test_cli_internal::ReplaceTestPreviewRenderer(
			    renderer, config, std::vector<howdy::native::EncodingModelInfo>{}, dependencies);
		} catch (const std::runtime_error &) {
			blocked = true;
		}
		renderer->Initialize();
		context.fail_destroy_window = false;
		test_cli_internal::ReplaceTestPreviewRenderer(
		    renderer, config, std::vector<howdy::native::EncodingModelInfo>{}, dependencies);
		renderer->Initialize();

		bool ok = true;
		ok &= expect(blocked, "window cleanup failure blocks renderer replacement");
		ok &=
		    expect(context.clear_callback_calls == 1, "successful callback cleanup is not retried");
		ok &= expect(context.destroy_window_calls == 2, "failed window cleanup is retried once");
		ok &= expect(context.initialize_calls == 2,
		             "replacement waits for window cleanup completion");
		return ok;
	}

	auto CallbackFailureWindowSuccessRetriesOnlyCallback() -> bool {
		RendererLifecycleContext context;
		context.fail_clear_callback = true;
		howdy::native::VideoConfig                               config{};
		const test_cli_internal::TestPreviewRendererDependencies dependencies{
		    .context        = &context,
		    .initialize     = InitializeRenderer,
		    .clear_callback = ClearRendererCallback,
		    .destroy_window = DestroyRendererWindow,
		};
		std::optional<test_cli_internal::TestPreviewRenderer> renderer;
		test_cli_internal::TestPreviewRendererCleanup         cleanup(renderer);
		renderer.emplace(config, std::vector<howdy::native::EncodingModelInfo>{}, dependencies);
		renderer->Initialize();

		bool blocked = false;
		try {
			test_cli_internal::ReplaceTestPreviewRenderer(
			    renderer, config, std::vector<howdy::native::EncodingModelInfo>{}, dependencies);
		} catch (const std::runtime_error &) {
			blocked = true;
		}
		renderer->Initialize();
		context.fail_clear_callback = false;
		test_cli_internal::ReplaceTestPreviewRenderer(
		    renderer, config, std::vector<howdy::native::EncodingModelInfo>{}, dependencies);
		renderer->Initialize();

		bool ok = true;
		ok &= expect(blocked, "callback cleanup failure blocks renderer replacement");
		ok &= expect(context.clear_callback_calls == 2, "failed callback cleanup is retried once");
		ok &= expect(context.destroy_window_calls == 1, "successful window cleanup is not retried");
		ok &= expect(context.initialize_calls == 2,
		             "replacement waits for callback cleanup completion");
		return ok;
	}

	auto RendererDefaultCleanupIsOrderedAndIdempotent() -> bool {
		RendererLifecycleContext               context;
		howdy::native::VideoConfig             config{};
		test_cli_internal::TestPreviewRenderer renderer(config, {},
		                                                {.context        = &context,
		                                                 .initialize     = InitializeRenderer,
		                                                 .clear_callback = ClearRendererCallback,
		                                                 .destroy_window = DestroyRendererWindow});

		renderer.Shutdown();
		renderer.Initialize();
		renderer.Initialize();
		renderer.Shutdown();
		bool ok = true;
		ok &= expect(context.initialize_calls == 1, "renderer initializes once");
		ok &= expect(context.clear_callback_calls == 1, "renderer clears callback once");
		ok &= expect(context.destroy_window_calls == 1, "renderer destroys window once");
		ok &= expect(context.sequence ==
		                 std::vector<std::string>{"initialize", "clear_callback", "destroy_window"},
		             "renderer clears callback before destroying window");
		return ok;
	}

	auto RendererFailedInitializeRollsBack() -> bool {
		RendererLifecycleContext context;
		context.fail_initialize = true;
		howdy::native::VideoConfig             config{};
		test_cli_internal::TestPreviewRenderer renderer(config, {},
		                                                {.context        = &context,
		                                                 .initialize     = InitializeRenderer,
		                                                 .clear_callback = ClearRendererCallback,
		                                                 .destroy_window = DestroyRendererWindow});

		bool threw = false;
		try {
			renderer.Initialize();
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

	auto RendererShutdownFinishesAfterCallbackError() -> bool {
		RendererLifecycleContext context;
		context.fail_clear_callback = true;
		howdy::native::VideoConfig             config{};
		test_cli_internal::TestPreviewRenderer renderer(config, {},
		                                                {.context        = &context,
		                                                 .initialize     = InitializeRenderer,
		                                                 .clear_callback = ClearRendererCallback,
		                                                 .destroy_window = DestroyRendererWindow});
		renderer.Initialize();

		bool threw       = false;
		bool retry_threw = false;
		try {
			renderer.Shutdown();
		} catch (const std::runtime_error &) {
			threw = true;
		}
		try {
			renderer.Shutdown();
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

	auto RendererPartialInitializationPreservesPrimaryFailure() -> bool {
		RendererLifecycleContext context;
		context.fail_initialize     = true;
		context.fail_clear_callback = true;
		context.fail_destroy_window = true;
		howdy::native::VideoConfig             config{};
		test_cli_internal::TestPreviewRenderer renderer(config, {},
		                                                {.context        = &context,
		                                                 .initialize     = InitializeRenderer,
		                                                 .clear_callback = ClearRendererCallback,
		                                                 .destroy_window = DestroyRendererWindow});

		std::string error_message;
		try {
			renderer.Initialize();
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

	auto RendererAlreadyClosedWindowCleanupIsIdempotent() -> bool {
		RendererLifecycleContext context;
		context.fail_clear_callback = true;
		context.fail_destroy_window = true;
		howdy::native::VideoConfig             config{};
		test_cli_internal::TestPreviewRenderer renderer(config, {},
		                                                {.context        = &context,
		                                                 .initialize     = InitializeRenderer,
		                                                 .clear_callback = ClearRendererCallback,
		                                                 .destroy_window = DestroyRendererWindow});
		renderer.Initialize();

		std::string error_message;
		bool        retry_threw = false;
		try {
			renderer.Shutdown();
		} catch (const std::runtime_error &error) {
			error_message = error.what();
		}
		try {
			renderer.Shutdown();
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

	auto PresenterFailureSurvivesRealOwnerCleanupFailures() -> bool {
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
			                     .initialize     = InitializeRenderer,
			                     .clear_callback = ClearRendererCallback,
			                     .destroy_window = DestroyRendererWindow,
			                 });
			renderer->Initialize();
			test_cli_internal::RunWithPreviewCleanup(renderer, nullptr, ThrowPresenterFailure);
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

auto RunTestPreviewRendererTests() -> bool;

auto RunTestPreviewRendererTests() -> bool {
	bool ok = true;
	ok &= RendererOwnsConfigurationAndModels();
	ok &= RepeatedPreviewReplacementCleansInitializedRendererFirst();
	ok &= FailedRendererCleanupPreventsReplacement();
	ok &= CallbackSuccessWindowFailureRetriesOnlyWindow();
	ok &= CallbackFailureWindowSuccessRetriesOnlyCallback();
	ok &= RendererDefaultCleanupIsOrderedAndIdempotent();
	ok &= RendererFailedInitializeRollsBack();
	ok &= RendererShutdownFinishesAfterCallbackError();
	ok &= RendererPartialInitializationPreservesPrimaryFailure();
	ok &= RendererAlreadyClosedWindowCleanupIsIdempotent();
	ok &= PresenterFailureSurvivesRealOwnerCleanupFailures();
	return ok;
}
