#pragma once

#include "config/runtime_config.hpp"
#include "recorders/video_capture.hpp"

#include <chrono>
#include <cstdint>
#include <string>

#include <opencv2/core.hpp>

namespace howdy::native {

	using CaptureOpenFn          = bool (*)(void *context);
	using CaptureReadGrayFrameFn = bool (*)(void *context, cv::Mat &gray_frame);
	using CaptureErrorMessageFn  = std::string (*)(void *context);

	struct CaptureProperty {
		int    id    = 0;
		double value = 0.0;
	};

	using CaptureSetPropertyFn = bool (*)(void *context, CaptureProperty property);
	using CaptureNowFn         = std::chrono::steady_clock::time_point (*)(void *context);

	struct CompareCaptureDependencies {
		void                  *capture_context = nullptr;
		CaptureOpenFn          open_capture    = nullptr;
		CaptureReadGrayFrameFn read_gray_frame = nullptr;
		CaptureErrorMessageFn  error_message   = nullptr;
		CaptureSetPropertyFn   set_property    = nullptr;

		void        *clock_context = nullptr;
		CaptureNowFn now           = nullptr;
	};

	enum class CompareCaptureOpenStatus : std::uint8_t {
		kOk,
		kOpenFailed,
		kInvalidDependencies,
	};

	struct CompareCaptureOpenResult {
		CompareCaptureOpenStatus status = CompareCaptureOpenStatus::kInvalidDependencies;
		std::string              error_message;
	};

	enum class CompareCaptureFrameStatus : std::uint8_t {
		kFrameReady,
		kTimeout,
		kReadFailed,
		kNotOpen,
		kInvalidDependencies,
	};

	struct CompareCaptureFrameResult {
		CompareCaptureFrameStatus status = CompareCaptureFrameStatus::kInvalidDependencies;
		cv::Mat                   gray_frame;
		int                       frame_number = 0;
		std::string               error_message;
	};

	struct CompareCaptureStats {
		int    frames             = 0;
		int    black_frames       = 0;
		int    dark_frames        = 0;
		int    valid_frames       = 0;
		double dark_running_total = 0.0;
	};

	class CompareCaptureSession {
	public:
		explicit CompareCaptureSession(const VideoConfig &config);

		CompareCaptureSession(const VideoConfig &config, CompareCaptureDependencies dependencies);

		auto open() -> CompareCaptureOpenResult;
		auto next_frame() -> CompareCaptureFrameResult;
		void reset_timeout_clock();

		void record_black_frame();
		void record_dark_frame(float darkness);
		void record_ready_frame(float darkness);
		void restore_exposure();

		[[nodiscard]] auto stats() const -> const CompareCaptureStats &;

	private:
		[[nodiscard]] auto dependencies_valid() const -> bool;

		VideoConfig                config_;
		VideoCapture               capture_;
		CompareCaptureDependencies dependencies_;
		CompareCaptureStats        stats_;
		std::chrono::steady_clock::time_point
		     loop_start_{};  // NOLINT(readability-redundant-member-init)
		bool is_open_ = false;
	};

}  // namespace howdy::native
