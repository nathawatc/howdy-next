#include "recorders/video_capture.hpp"

#include "common/capture_device_path.hpp"
#include "config/config_values.hpp"

#include <filesystem>
#include <string>
#include <utility>

#include <opencv2/imgproc.hpp>

namespace howdy::native {

	namespace {

		constexpr auto kNoDevice = "none";

	}  // namespace

	auto load_capture_settings(const ConfigReader &config) -> CaptureSettings {
		return CaptureSettings{
		    .device_path    = config.get("video", "device_path", "/dev/video0"),
		    .warn_no_device = config.get_bool("video", "warn_no_device", true),
		    .force_mjpeg    = config.get_bool("video", "force_mjpeg", false),
		    .frame_width    = config_frame_width(config),
		    .frame_height   = config_frame_height(config),
		    .device_fps     = config_device_fps(config),
		};
	}

	VideoCapture::VideoCapture(CaptureSettings settings)
	    : settings_(std::move(settings)) {}

	auto VideoCapture::open() -> bool {
		release();
		error_ = CaptureError::kNone;
		error_message_.clear();

		if (settings_.device_path != kNoDevice && !std::filesystem::exists(settings_.device_path)) {
			if (settings_.warn_no_device) {
				set_error(CaptureError::kMissingDevice,
				          "Configured camera device does not exist: " + settings_.device_path);
				return false;
			}
		}

		if (!is_allowed_capture_device_path(settings_.device_path)) {
			set_error(CaptureError::kOpenFailed, "Configured camera device must be a /dev/video* "
			                                     "or /dev/v4l/by-path/* character device: " +
			                                         settings_.device_path);
			return false;
		}

		try {
			capture_.open(settings_.device_path, cv::CAP_V4L);
			if (!capture_.isOpened()) {
				set_error(CaptureError::kOpenFailed,
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

			// Keep a warm-up grab for compatibility with existing behavior; some
			// devices may fail the first grab immediately after open and recover on
			// subsequent reads.
			(void)grab();
			return true;
		} catch (const cv::Exception &error) {
			release();
			set_error(CaptureError::kOpenFailed, "OpenCV failed while opening camera device " +
			                                         settings_.device_path + ": " + error.what());
			return false;
		}
	}

	auto VideoCapture::grab() -> bool {
		if (!capture_.isOpened()) {
			set_error(CaptureError::kOpenFailed, "Camera is not open");
			return false;
		}

		if (!capture_.grab()) {
			set_error(CaptureError::kReadFailed, "Failed to grab a frame from camera");
			return false;
		}
		return true;
	}

	auto VideoCapture::read(cv::Mat &frame, cv::Mat *gray_frame) -> bool {
		if (!capture_.isOpened()) {
			set_error(CaptureError::kOpenFailed, "Camera is not open");
			return false;
		}

		try {
			if (!capture_.read(frame) || frame.empty()) {
				set_error(CaptureError::kReadFailed, "Failed to read a frame from camera");
				return false;
			}
			if (frame.rows <= 0 || frame.cols <= 0) {
				set_error(CaptureError::kReadFailed, "Camera returned invalid frame dimensions");
				return false;
			}

			if (gray_frame != nullptr) {
				if (frame.channels() == 3) {
					cv::cvtColor(frame, *gray_frame, cv::COLOR_BGR2GRAY);
				} else if (frame.channels() == 4) {
					cv::cvtColor(frame, *gray_frame, cv::COLOR_BGRA2GRAY);
				} else if (frame.channels() == 1) {
					*gray_frame = frame;
				} else {
					set_error(CaptureError::kReadFailed,
					          "Camera returned unsupported frame channel count: " +
					              std::to_string(frame.channels()));
					return false;
				}
			}

			return true;
		} catch (const cv::Exception &error) {
			set_error(CaptureError::kReadFailed,
			          "OpenCV failed while reading camera frame: " + std::string(error.what()));
			return false;
		}
	}

	void VideoCapture::release() {
		if (capture_.isOpened()) {
			capture_.release();
		}
	}

	auto VideoCapture::is_open() const -> bool {
		return capture_.isOpened();
	}

	auto VideoCapture::get(int property) const -> double {
		return capture_.isOpened() ? capture_.get(property) : 0.0;
	}

	auto VideoCapture::set(int property, double value) -> bool {
		return capture_.isOpened() && capture_.set(property, value);
	}

	auto VideoCapture::error() const -> CaptureError {
		return error_;
	}

	auto VideoCapture::error_message() const -> const std::string & {
		return error_message_;
	}

	auto VideoCapture::settings() const -> const CaptureSettings & {
		return settings_;
	}

	void VideoCapture::set_error(CaptureError error, std::string message) {
		error_         = error;
		error_message_ = std::move(message);
	}

}  // namespace howdy::native
