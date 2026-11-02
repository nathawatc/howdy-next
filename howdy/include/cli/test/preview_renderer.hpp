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
		friend class TestPreviewRendererTestAccess;

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
		TestPreviewRendererCleanup(const TestPreviewRendererCleanup &)                     = delete;
		auto operator=(const TestPreviewRendererCleanup &) -> TestPreviewRendererCleanup & = delete;
		TestPreviewRendererCleanup(TestPreviewRendererCleanup &&)                          = delete;
		auto operator=(TestPreviewRendererCleanup &&) -> TestPreviewRendererCleanup &      = delete;
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

	class PreviewCleanup {
	public:
		PreviewCleanup(std::optional<TestPreviewRenderer>         &renderer,
		               std::optional<howdy::native::VideoCapture> &preview_capture);
		PreviewCleanup(const PreviewCleanup &)                     = delete;
		auto operator=(const PreviewCleanup &) -> PreviewCleanup & = delete;
		PreviewCleanup(PreviewCleanup &&)                          = delete;
		auto operator=(PreviewCleanup &&) -> PreviewCleanup &      = delete;
		~PreviewCleanup() noexcept;

	private:
		std::optional<howdy::native::VideoCapture> &capture_;
		TestPreviewRendererCleanup                  renderer_cleanup_;
	};

}  // namespace howdy::native::test_cli_internal
