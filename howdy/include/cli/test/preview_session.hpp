#pragma once

#include "cli/test/internal.hpp"
#include "config/runtime_config.hpp"
#include "vision/preview_engine.hpp"

#include <chrono>

#include <opencv2/core.hpp>

namespace howdy::native::test_cli_internal {

	struct TestPreviewFrameStats {
		int total_frames = 0;
		int fps          = 0;
	};

	using ReadPreviewGrayFrameFn   = bool (*)(void *context, cv::Mat &gray_frame);
	using RestorePreviewExposureFn = void (*)(void *context);
	using PresentPreviewFrameFn    = bool (*)(void                                    *context,
	                                          const howdy::native::PreviewFrameResult &frame_result,
	                                          const TestPreviewFrameStats             &stats);
	using PreviewSlowModeFn        = bool (*)(void *context);
	using PreviewNowFn             = std::chrono::steady_clock::time_point (*)(void *context);
	using PreviewSleepFn           = void (*)(void *context, std::chrono::milliseconds duration);

	struct TestPreviewSessionDependencies {
		void                    *capture_context  = nullptr;
		ReadPreviewGrayFrameFn   read_gray_frame  = nullptr;
		RestorePreviewExposureFn restore_exposure = nullptr;

		void                 *renderer_context = nullptr;
		PresentPreviewFrameFn present          = nullptr;
		PreviewSlowModeFn     slow_mode        = nullptr;

		void          *clock_context = nullptr;
		PreviewNowFn   now           = nullptr;
		void          *sleep_context = nullptr;
		PreviewSleepFn sleep         = nullptr;
	};

	class TestPreviewSession {
	public:
		TestPreviewSession(const howdy::native::VideoConfig &config,
		                   howdy::native::PreviewEngine     &preview_engine,
		                   TestPreviewSessionDependencies    dependencies);

		auto run(cv::Mat prefetched_gray_frame) -> TestPreviewResult;

	private:
		[[nodiscard]] auto dependencies_valid() const -> bool;

		const howdy::native::VideoConfig &config_;
		howdy::native::PreviewEngine     &preview_engine_;
		TestPreviewSessionDependencies    dependencies_;
	};

	auto run_preview_session_with_retained_frame(TestPreviewSession &session,
	                                             const cv::Mat      &prefetched_gray_frame)
	    -> TestPreviewResult;
	auto run_preview_session_with_retained_frame(TestPreviewSession &session,
	                                             cv::Mat           &&prefetched_gray_frame)
	    -> TestPreviewResult = delete;

}  // namespace howdy::native::test_cli_internal
