#pragma once

#include "config/runtime_config.hpp"

#include <cstdint>
#include <memory>
#include <string>

#include <opencv2/videoio.hpp>

namespace howdy::native {

	enum class CaptureError : std::uint8_t {
		kNone,
		kMissingDevice,
		kOpenFailed,
		kReadFailed,
	};

	struct CaptureSettings {
		std::string device_path;
		bool        warn_no_device = true;
		bool        force_mjpeg    = false;
		int         frame_width    = -1;
		int         frame_height   = -1;
		int         device_fps     = 0;
	};

	auto load_capture_settings(const VideoConfig &config) -> CaptureSettings;

	class VideoCaptureTestAccess;

	class VideoCapture {
	public:
		explicit VideoCapture(CaptureSettings settings);

		auto open(bool perform_warm_up = true) -> bool;
		auto warm_up() -> bool;
		auto grab() -> bool;
		auto read(cv::Mat &frame, cv::Mat *gray_frame = nullptr) -> bool;
		void release();

		[[nodiscard]] auto is_open() const -> bool;
		[[nodiscard]] auto get(int property) const -> double;
		auto               set(int property, double value) -> bool;

		[[nodiscard]] auto error() const -> CaptureError;
		[[nodiscard]] auto error_message() const -> const std::string &;
		[[nodiscard]] auto settings() const -> const CaptureSettings &;

	private:
		class FrameReader {
		public:
			virtual ~FrameReader()                    = default;
			virtual auto read(cv::Mat &frame) -> bool = 0;
		};

		VideoCapture(CaptureSettings settings, std::shared_ptr<FrameReader> frame_reader);

		void set_error(CaptureError error, std::string message);

		friend class VideoCaptureTestAccess;

		CaptureSettings              settings_;
		cv::VideoCapture             capture_;
		std::shared_ptr<FrameReader> frame_reader_;
		CaptureError                 error_ = CaptureError::kNone;
		std::string                  error_message_;
	};

}  // namespace howdy::native
