#pragma once

#include "cli/test/preview_session.hpp"
#include "config/runtime_config.hpp"
#include "storage/user_model_types.hpp"
#include "vision/video_capture.hpp"

#include <optional>
#include <string>
#include <vector>

namespace howdy::native::test_cli_internal {
	struct PreviewRendererInitialization {
		void *context  = nullptr;
		void *renderer = nullptr;
	};

	using InitializePreviewRendererFn = void (*)(PreviewRendererInitialization initialization);
	using PreviewRendererCleanupFn    = void (*)(void *context);

	struct TestPreviewRendererDependencies {
		void                       *context        = nullptr;
		InitializePreviewRendererFn initialize     = nullptr;
		PreviewRendererCleanupFn    clear_callback = nullptr;
		PreviewRendererCleanupFn    destroy_window = nullptr;
	};

	class TestPreviewRenderer {
	public:
		TestPreviewRenderer(howdy::native::VideoConfig                    config,
		                    std::vector<howdy::native::EncodingModelInfo> models,
		                    TestPreviewRendererDependencies               dependencies = {});

		void Initialize();
		void Shutdown();

		auto Present(const howdy::native::PreviewFrameResult &frame_result,
		             const TestPreviewFrameStats             &stats) -> bool;

		[[nodiscard]] auto SlowMode() const -> bool;

	private:
		// OpenCV MouseCallback requires int, int, int, int, void *.
		// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
		static void MouseCallback(int event, int x, int y, int flags, void *userdata);
		static void PrintText(cv::Mat &overlay, int line_number, int height,
		                      const std::string &text);

		howdy::native::VideoConfig                    config_;
		std::vector<howdy::native::EncodingModelInfo> models_;
		TestPreviewRendererDependencies               dependencies_;
		bool                                          slow_mode_                = false;
		bool                                          initialized_              = false;
		bool                                          callback_cleanup_pending_ = false;
		bool                                          window_cleanup_pending_   = false;
	};

	class TestPreviewRendererCleanup {
	public:
		explicit TestPreviewRendererCleanup(std::optional<TestPreviewRenderer> &renderer);
		~TestPreviewRendererCleanup() noexcept;
		void Cleanup();

	private:
		std::optional<TestPreviewRenderer> &renderer_;
		bool                                cleanup_attempted_ = false;
	};

	void ReplaceTestPreviewRenderer(std::optional<TestPreviewRenderer>           &renderer,
	                                howdy::native::VideoConfig                    config,
	                                std::vector<howdy::native::EncodingModelInfo> models,
	                                TestPreviewRendererDependencies dependencies = {});

	using PreviewCleanupBodyFn = void (*)(void *context);

	struct PreviewCleanup {
		std::optional<howdy::native::VideoCapture> &capture;
		TestPreviewRendererCleanup                  renderer_cleanup;

		PreviewCleanup(std::optional<TestPreviewRenderer>         &renderer,
		               std::optional<howdy::native::VideoCapture> &preview_capture);
		~PreviewCleanup() noexcept;
	};

	void RunWithPreviewCleanup(std::optional<TestPreviewRenderer> &renderer, void *context,
	                           PreviewCleanupBodyFn body);

}  // namespace howdy::native::test_cli_internal
