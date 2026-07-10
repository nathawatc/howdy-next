#include "common/compare_capture_session.hpp"

#include <chrono>
#include <utility>

#include <opencv2/videoio.hpp>

namespace {

	auto open_capture_dependency(void *context) -> bool {
		return static_cast<howdy::native::VideoCapture *>(context)->open();
	}

	auto read_gray_frame_dependency(void *context, cv::Mat &gray_frame) -> bool {
		cv::Mat frame;
		return static_cast<howdy::native::VideoCapture *>(context)->read(frame, &gray_frame);
	}

	auto capture_error_message_dependency(void *context) -> std::string {
		return static_cast<howdy::native::VideoCapture *>(context)->error_message();
	}

	auto set_capture_property_dependency(void *context, int property, double value) -> bool {
		return static_cast<howdy::native::VideoCapture *>(context)->set(property, value);
	}

	auto steady_now_dependency([[maybe_unused]] void *context)
	    -> std::chrono::steady_clock::time_point {
		return std::chrono::steady_clock::now();
	}

}  // namespace

namespace howdy::native {

	CompareCaptureSession::CompareCaptureSession(
	    const VideoConfig &config)  // NOLINT(modernize-pass-by-value)
	    : config_(config)
	    , capture_(load_capture_settings(config_)) {
		dependencies_ = {
		    .capture_context = &capture_,
		    .open_capture    = open_capture_dependency,
		    .read_gray_frame = read_gray_frame_dependency,
		    .error_message   = capture_error_message_dependency,
		    .set_property    = set_capture_property_dependency,
		    .clock_context   = nullptr,
		    .now             = steady_now_dependency,
		};
	}

	CompareCaptureSession::CompareCaptureSession(
	    const VideoConfig         &config,  // NOLINT(modernize-pass-by-value)
	    CompareCaptureDependencies dependencies)
	    : config_(config)
	    , capture_(load_capture_settings(config_))
	    , dependencies_(dependencies) {}

	auto CompareCaptureSession::dependencies_valid() const -> bool {
		return dependencies_.capture_context != nullptr && dependencies_.open_capture != nullptr &&
		       dependencies_.read_gray_frame != nullptr && dependencies_.error_message != nullptr &&
		       (config_.exposure == -1 || dependencies_.set_property != nullptr) &&
		       dependencies_.now != nullptr;
	}

	auto CompareCaptureSession::open() -> CompareCaptureOpenResult {
		is_open_ = false;
		if (!dependencies_valid()) {
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

		is_open_    = true;
		stats_      = {};
		loop_start_ = dependencies_.now(dependencies_.clock_context);
		return {
		    .status = CompareCaptureOpenStatus::kOk,
		};
	}

	auto CompareCaptureSession::next_frame() -> CompareCaptureFrameResult {
		if (!dependencies_valid()) {
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

	void CompareCaptureSession::reset_timeout_clock() {
		if (!is_open_ || !dependencies_valid()) {
			return;
		}

		loop_start_ = dependencies_.now(dependencies_.clock_context);
	}

	void CompareCaptureSession::record_black_frame() {
		stats_.black_frames++;
	}

	void CompareCaptureSession::record_dark_frame(float darkness) {
		stats_.dark_running_total += darkness;
		stats_.valid_frames++;
		stats_.dark_frames++;
	}

	void CompareCaptureSession::record_ready_frame(float darkness) {
		stats_.dark_running_total += darkness;
		stats_.valid_frames++;
	}

	void
	CompareCaptureSession::restore_exposure() {  // NOLINT(readability-make-member-function-const)
		if (!is_open_ || !dependencies_valid() || config_.exposure == -1) {
			return;
		}

		(void)dependencies_.set_property(dependencies_.capture_context, cv::CAP_PROP_AUTO_EXPOSURE,
		                                 1.0);
		(void)dependencies_.set_property(dependencies_.capture_context, cv::CAP_PROP_EXPOSURE,
		                                 static_cast<double>(config_.exposure));
	}

	auto CompareCaptureSession::stats() const -> const CompareCaptureStats & {
		return stats_;
	}

}  // namespace howdy::native
