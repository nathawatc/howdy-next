#include "compare/capture_session.hpp"

#include "config/runtime_config.hpp"

#include <chrono>
#include <utility>

#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>

namespace {

	auto OpenCaptureDependency(void *context) -> bool {
		return static_cast<howdy::native::VideoCapture *>(context)->Open(false);
	}

	auto WarmUpCaptureDependency(void *context) -> bool {
		return static_cast<howdy::native::VideoCapture *>(context)->WarmUp();
	}

	auto ReadGrayFrameDependency(void *context, cv::Mat &gray_frame) -> bool {
		cv::Mat frame;
		return static_cast<howdy::native::VideoCapture *>(context)->Read(frame, &gray_frame);
	}

	auto CaptureErrorMessageDependency(void *context) -> std::string {
		return static_cast<howdy::native::VideoCapture *>(context)->ErrorMessage();
	}

	auto SetCapturePropertyDependency(void *context, howdy::native::CaptureProperty property)
	    -> bool {
		return static_cast<howdy::native::VideoCapture *>(context)->Set(property.id,
		                                                                property.value);
	}

	auto SteadyNowDependency([[maybe_unused]] void *context)
	    -> std::chrono::steady_clock::time_point {
		return std::chrono::steady_clock::now();
	}

}  // namespace

namespace howdy::native {

	CompareCaptureSession::CompareCaptureSession(
	    const VideoConfig &config)  // NOLINT(modernize-pass-by-value)
	    : config_(config)
	    , capture_(LoadCaptureSettings(config_)) {
		dependencies_ = {
		    .capture_context = &capture_,
		    .open_capture    = OpenCaptureDependency,
		    .warm_up_capture = WarmUpCaptureDependency,
		    .read_gray_frame = ReadGrayFrameDependency,
		    .error_message   = CaptureErrorMessageDependency,
		    .set_property    = SetCapturePropertyDependency,
		    .clock_context   = nullptr,
		    .now             = SteadyNowDependency,
		};
	}

	CompareCaptureSession::CompareCaptureSession(
	    const VideoConfig         &config,  // NOLINT(modernize-pass-by-value)
	    CompareCaptureDependencies dependencies)
	    : config_(config)
	    , capture_(LoadCaptureSettings(config_))
	    , dependencies_(dependencies) {}

	auto CompareCaptureSession::DependenciesValid() const -> bool {
		return dependencies_.capture_context != nullptr && dependencies_.open_capture != nullptr &&
		       dependencies_.warm_up_capture != nullptr &&
		       dependencies_.read_gray_frame != nullptr && dependencies_.error_message != nullptr &&
		       (config_.exposure == -1 || dependencies_.set_property != nullptr) &&
		       dependencies_.now != nullptr;
	}

	auto CompareCaptureSession::Open() -> CompareCaptureOpenResult {
		is_open_ = false;
		if (!DependenciesValid()) {
			return {
			    .status = CompareCaptureOpenStatus::kInvalidDependencies,
			};
		}

		if (!dependencies_.open_capture(dependencies_.capture_context)) {
			return {
			    .status        = CompareCaptureOpenStatus::kOpenFailed,
			    .error_message = dependencies_.error_message(dependencies_.capture_context),
			};
		}

		is_open_ = true;
		stats_   = {};
		RestoreExposure();
		if (!dependencies_.warm_up_capture(dependencies_.capture_context)) {
			is_open_ = false;
			return {
			    .status        = CompareCaptureOpenStatus::kOpenFailed,
			    .error_message = dependencies_.error_message(dependencies_.capture_context),
			};
		}
		loop_start_ = dependencies_.now(dependencies_.clock_context);
		return {
		    .status = CompareCaptureOpenStatus::kOk,
		};
	}

	auto CompareCaptureSession::NextFrame() -> CompareCaptureFrameResult {
		if (!DependenciesValid()) {
			return {
			    .status = CompareCaptureFrameStatus::kInvalidDependencies,
			};
		}
		if (!is_open_) {
			return {
			    .status = CompareCaptureFrameStatus::kNotOpen,
			};
		}

		stats_.frames++;
		if (dependencies_.now(dependencies_.clock_context) - loop_start_ >=
		    std::chrono::seconds(config_.timeout)) {
			return {
			    .status       = CompareCaptureFrameStatus::kTimeout,
			    .frame_number = stats_.frames,
			};
		}

		cv::Mat gray_frame;
		if (!dependencies_.read_gray_frame(dependencies_.capture_context, gray_frame)) {
			return {
			    .status        = CompareCaptureFrameStatus::kReadFailed,
			    .frame_number  = stats_.frames,
			    .error_message = dependencies_.error_message(dependencies_.capture_context),
			};
		}

		return {
		    .status       = CompareCaptureFrameStatus::kFrameReady,
		    .gray_frame   = std::move(gray_frame),
		    .frame_number = stats_.frames,
		};
	}

	void CompareCaptureSession::ResetTimeoutClock() {
		if (!is_open_ || !DependenciesValid()) {
			return;
		}

		loop_start_ = dependencies_.now(dependencies_.clock_context);
	}

	void CompareCaptureSession::RecordBlackFrame() {
		stats_.black_frames++;
	}

	void CompareCaptureSession::RecordDarkFrame(float darkness) {
		stats_.dark_running_total += darkness;
		stats_.valid_frames++;
		stats_.dark_frames++;
	}

	void CompareCaptureSession::RecordReadyFrame(float darkness) {
		stats_.dark_running_total += darkness;
		stats_.valid_frames++;
	}

	void
	CompareCaptureSession::RestoreExposure() {  // NOLINT(readability-make-member-function-const)
		if (!is_open_ || !DependenciesValid() || config_.exposure == -1) {
			return;
		}

		try {
			(void)dependencies_.set_property(dependencies_.capture_context,
			                                 {.id = cv::CAP_PROP_AUTO_EXPOSURE, .value = 1.0});
		} catch (const cv::Exception &) {  // NOLINT(bugprone-empty-catch)
			// Camera may disconnect during restoration; continue with next property.
		}

		try {
			(void)dependencies_.set_property(
			    dependencies_.capture_context,
			    {.id = cv::CAP_PROP_EXPOSURE, .value = static_cast<double>(config_.exposure)});
		} catch (const cv::Exception &) {  // NOLINT(bugprone-empty-catch)
			// Camera may disconnect during restoration; restoration is best effort.
		}
	}

	auto CompareCaptureSession::Stats() const -> const CompareCaptureStats & {
		return stats_;
	}

}  // namespace howdy::native
