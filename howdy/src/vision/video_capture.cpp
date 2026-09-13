#include "vision/video_capture.hpp"

#include "config/runtime_config.hpp"
#include "support/capture_device_path.hpp"
#include "vision/frame_validation.hpp"

#include <filesystem>
#include <string>
#include <utility>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

namespace howdy::native {

	namespace {

		constexpr auto kCameraNotOpenMessage = "Camera is not open";

	}  // namespace

	auto LoadCaptureSettings(const VideoConfig &config) -> CaptureSettings {
		return CaptureSettings{
		    .device_path    = config.device_path,
		    .warn_no_device = config.warn_no_device,
		    .force_mjpeg    = config.force_mjpeg,
		    .frame_width    = config.frame_width,
		    .frame_height   = config.frame_height,
		    .device_fps     = config.device_fps,
		};
	}

	VideoCapture::VideoCapture(CaptureSettings settings)
	    : settings_(std::move(settings)) {}

	VideoCapture::VideoCapture(CaptureSettings settings, std::shared_ptr<FrameReader> frame_reader)
	    : settings_(std::move(settings))
	    , frame_reader_(std::move(frame_reader)) {}

	auto VideoCapture::Open(bool perform_warm_up) -> bool {
		if (settings_.device_path == kNoCaptureDevice) {
			SetError(CaptureError::kMissingDevice,
			         "Camera is not configured; set video.device_path");
			return false;
		}

		Release();
		error_ = CaptureError::kNone;
		error_message_.clear();

		if (!std::filesystem::exists(settings_.device_path)) {
			if (settings_.warn_no_device) {
				SetError(CaptureError::kMissingDevice,
				         "Configured camera device does not exist: " + settings_.device_path);
				return false;
			}
		}

		if (!IsAllowedCaptureDevicePath(settings_.device_path)) {
			SetError(CaptureError::kOpenFailed,
			         "Configured camera device must be a /dev/video*, /dev/v4l/by-path/*, "
			         "or /dev/v4l/by-id/* character device: " +
			             settings_.device_path);
			return false;
		}

		try {
			capture_.open(settings_.device_path, cv::CAP_V4L);
			if (!capture_.isOpened()) {
				SetError(CaptureError::kOpenFailed,
				         "Failed to open camera device: " + settings_.device_path);
				return false;
			}

			if (settings_.device_fps > 0) {
				capture_.set(cv::CAP_PROP_FPS, settings_.device_fps);
			}
			if (settings_.force_mjpeg) {
				capture_.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
			}
			if (settings_.frame_width != -1) {
				capture_.set(cv::CAP_PROP_FRAME_WIDTH, settings_.frame_width);
			}
			if (settings_.frame_height != -1) {
				capture_.set(cv::CAP_PROP_FRAME_HEIGHT, settings_.frame_height);
			}

			if (perform_warm_up && !WarmUp()) {
				return false;
			}
			return true;
		} catch (const cv::Exception &error) {
			Release();
			SetError(CaptureError::kOpenFailed, "OpenCV failed while opening camera device " +
			                                        settings_.device_path + ": " + error.what());
			return false;
		}
	}

	auto VideoCapture::WarmUp() -> bool {
		try {
			// Keep a warm-up grab for compatibility with existing behavior; some
			// devices may fail the first grab immediately after open and recover on
			// subsequent reads.
			(void)Grab();
			return true;
		} catch (const cv::Exception &error) {
			Release();
			SetError(CaptureError::kOpenFailed, "OpenCV failed while opening camera device " +
			                                        settings_.device_path + ": " + error.what());
			return false;
		}
	}

	auto VideoCapture::Grab() -> bool {
		if (!capture_.isOpened()) {
			SetError(CaptureError::kOpenFailed, kCameraNotOpenMessage);
			return false;
		}

		if (!capture_.grab()) {
			SetError(CaptureError::kReadFailed, "Failed to grab a frame from camera");
			return false;
		}
		return true;
	}

	auto VideoCapture::Read(cv::Mat &frame, cv::Mat *gray_frame) -> bool {
		if (!frame_reader_ && !capture_.isOpened()) {
			SetError(CaptureError::kOpenFailed, kCameraNotOpenMessage);
			return false;
		}

		try {
			const bool read_ok = frame_reader_ ? frame_reader_->Read(frame) : capture_.read(frame);
			if (!read_ok) {
				SetError(CaptureError::kReadFailed, kCameraReadFailureMessage);
				return false;
			}

			switch (ValidateFrame(frame, FrameChannelPolicy::kCameraInput)) {
				case FrameValidationStatus::kValid:
					break;
				case FrameValidationStatus::kEmpty:
					SetError(CaptureError::kReadFailed, "Camera returned an empty frame");
					return false;
				case FrameValidationStatus::kUnsupportedDimensions:
					SetError(CaptureError::kReadFailed,
					         "Camera returned unsupported frame dimensions: " +
					             std::to_string(frame.dims));
					return false;
				case FrameValidationStatus::kOversizedDimensions:
					SetError(CaptureError::kReadFailed,
					         "Camera returned oversized frame dimensions: " +
					             std::to_string(frame.cols) + "x" + std::to_string(frame.rows) +
					             " (max supported dimension: " +
					             std::to_string(kMaxFrameDimension) + ")");
					return false;
				case FrameValidationStatus::kUnsupportedChannelCount:
					SetError(CaptureError::kReadFailed,
					         "Camera returned unsupported frame channel count: " +
					             std::to_string(frame.channels()));
					return false;
				case FrameValidationStatus::kUnsupportedPixelType:
					SetError(CaptureError::kReadFailed,
					         "Camera returned unsupported frame pixel type: " +
					             std::to_string(frame.type()));
					return false;
			}

			if (gray_frame != nullptr) {
				if (frame.channels() == 3) {
					cv::cvtColor(frame, *gray_frame, cv::COLOR_BGR2GRAY);
				} else if (frame.channels() == 4) {
					cv::cvtColor(frame, *gray_frame, cv::COLOR_BGRA2GRAY);
				} else if (frame.channels() == 1) {
					*gray_frame = frame;
				}
			}

			return true;
		} catch (const cv::Exception &error) {
			SetError(CaptureError::kReadFailed,
			         "OpenCV failed while reading camera frame: " + std::string(error.what()));
			return false;
		}
	}

	void VideoCapture::Release() {
		if (capture_.isOpened()) {
			capture_.release();
		}
	}

	auto VideoCapture::IsOpen() const -> bool {
		return capture_.isOpened();
	}

	auto VideoCapture::Get(int property) const -> double {
		return capture_.isOpened() ? capture_.get(property) : 0.0;
	}

	auto VideoCapture::Set(int property, double value) -> bool {
		return capture_.isOpened() && capture_.set(property, value);
	}

	auto VideoCapture::Error() const -> CaptureError {
		return error_;
	}

	auto VideoCapture::ErrorMessage() const -> const std::string & {
		return error_message_;
	}

	auto VideoCapture::Settings() const -> const CaptureSettings & {
		return settings_;
	}

	void VideoCapture::SetError(CaptureError error, std::string message) {
		error_         = error;
		error_message_ = std::move(message);
	}

}  // namespace howdy::native
