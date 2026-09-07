#include "cli/test/preview_session.hpp"

#include "config/runtime_config.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

#include <opencv2/core.hpp>

namespace howdy::native::test_cli_internal {
	namespace {
		class ExposureRestoreGuard {
		public:
			ExposureRestoreGuard(const VideoConfig                    &config,
			                     const TestPreviewSessionDependencies &dependencies)
			    : dependencies_(config.exposure == -1 ? nullptr : &dependencies) {}

			~ExposureRestoreGuard() noexcept {
				if (dependencies_ == nullptr) {
					return;
				}
				try {
					dependencies_->restore_exposure(dependencies_->capture_context);
				} catch (...) {  // NOLINT(bugprone-empty-catch)
				}
			}

		private:
			const TestPreviewSessionDependencies *dependencies_;
		};
	}  // namespace

	TestPreviewSession::TestPreviewSession(const howdy::native::VideoConfig &config,
	                                       howdy::native::PreviewEngine     &preview_engine,
	                                       TestPreviewSessionDependencies    dependencies)
	    : config_(config)
	    , preview_engine_(preview_engine)
	    , dependencies_(dependencies) {}

	auto TestPreviewSession::DependenciesValid() const -> bool {
		return dependencies_.capture_context != nullptr &&
		       dependencies_.read_gray_frame != nullptr &&
		       (config_.exposure == -1 || dependencies_.restore_exposure != nullptr) &&
		       dependencies_.renderer_context != nullptr && dependencies_.present != nullptr &&
		       dependencies_.slow_mode != nullptr && dependencies_.now != nullptr &&
		       dependencies_.sleep != nullptr;
	}

	auto TestPreviewSession::Run(cv::Mat prefetched_gray_frame) -> TestPreviewResult {
		if (!DependenciesValid()) {
			return {
			    .status        = TestPreviewStatus::kFaceModelError,
			    .error_message = "Internal error: missing test preview session dependency",
			};
		}

		bool has_prefetched = true;
		int  total_frames   = 0;
		int  sec_frames     = 0;
		int  fps            = 0;
		auto second_anchor  = dependencies_.now(dependencies_.clock_context);

		while (true) {
			ExposureRestoreGuard exposure_restore(config_, dependencies_);
			const auto           frame_start = dependencies_.now(dependencies_.clock_context);
			total_frames++;
			sec_frames++;

			if (std::chrono::duration_cast<std::chrono::seconds>(frame_start - second_anchor)
			        .count() >= 1) {
				fps           = sec_frames;
				sec_frames    = 0;
				second_anchor = frame_start;
			}

			cv::Mat gray_frame;
			if (has_prefetched) {
				gray_frame = prefetched_gray_frame;
				prefetched_gray_frame.release();
				has_prefetched = false;
			} else if (!dependencies_.read_gray_frame(dependencies_.capture_context, gray_frame)) {
				return {.status = TestPreviewStatus::kCameraReadError};
			}

			auto frame_result  = preview_engine_.ProcessGrayFrame(std::move(gray_frame));
			auto frame_failure = MapPreviewFrameFailure(frame_result);
			if (frame_failure.status != TestPreviewStatus::kOk) {
				return frame_failure;
			}

			if (!dependencies_.present(dependencies_.renderer_context, frame_result,
			                           {.total_frames = total_frames, .fps = fps})) {
				break;
			}

			const auto frame_time =
			    std::chrono::duration_cast<std::chrono::milliseconds>(
			        dependencies_.now(dependencies_.clock_context) - frame_start)
			        .count();
			if (dependencies_.slow_mode(dependencies_.renderer_context)) {
				const auto sleep_ms = std::max(0LL, 500LL - frame_time);
				dependencies_.sleep(dependencies_.sleep_context,
				                    std::chrono::milliseconds(sleep_ms));
			}
		}

		return {.status = TestPreviewStatus::kOk};
	}

	auto RunPreviewSessionWithRetainedFrame(TestPreviewSession &session,
	                                        const cv::Mat      &prefetched_gray_frame)
	    -> TestPreviewResult {
		return session.Run(prefetched_gray_frame);
	}

	auto MapPreviewFrameFailure(const howdy::native::PreviewFrameResult &result)
	    -> TestPreviewResult {
		switch (result.status) {
			case howdy::native::PreviewFrameStatus::kInvalidFrame:
				return {
				    .status        = TestPreviewStatus::kCameraReadError,
				    .error_message = result.error_message,
				};
			case howdy::native::PreviewFrameStatus::kDetectionFailed:
			case howdy::native::PreviewFrameStatus::kEncodingFailed:
			case howdy::native::PreviewFrameStatus::kInvalidMatchResult:
			case howdy::native::PreviewFrameStatus::kInvalidDependencies:
				return {
				    .status        = TestPreviewStatus::kFaceModelError,
				    .error_message = result.error_message,
				};
			default:
				return {.status = TestPreviewStatus::kOk};
		}
	}

}  // namespace howdy::native::test_cli_internal
