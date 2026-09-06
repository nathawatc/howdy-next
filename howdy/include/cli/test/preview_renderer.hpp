#pragma once

#include "cli/test/preview_session.hpp"
#include "config/runtime_config.hpp"
#include "storage/user_models.hpp"

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

		void initialize();
		void shutdown();

		auto present(const howdy::native::PreviewFrameResult &frame_result,
		             const TestPreviewFrameStats             &stats) -> bool;

		[[nodiscard]] auto slow_mode() const -> bool;

	private:
		// OpenCV MouseCallback requires int, int, int, int, void *.
		// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
		static void mouse_callback(int event, int x, int y, int flags, void *userdata);
		static void print_text(cv::Mat &overlay, int line_number, int height,
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
		void cleanup();

	private:
		std::optional<TestPreviewRenderer> &renderer_;
		bool                                cleanup_attempted_ = false;
	};

	void replace_test_preview_renderer(std::optional<TestPreviewRenderer>           &renderer,
	                                   howdy::native::VideoConfig                    config,
	                                   std::vector<howdy::native::EncodingModelInfo> models,
	                                   TestPreviewRendererDependencies dependencies = {});

}  // namespace howdy::native::test_cli_internal
