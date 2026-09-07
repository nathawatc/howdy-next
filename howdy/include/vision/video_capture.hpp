#pragma once

#include "config/runtime_config.hpp"

#include <cstdint>
#include <memory>
#include <string>

#include <opencv2/videoio.hpp>

namespace howdy::native {
	inline constexpr auto kCameraReadFailureMessage = "Could not capture a camera frame";

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

	auto LoadCaptureSettings(const VideoConfig &config) -> CaptureSettings;

	class VideoCaptureTestAccess;

	class VideoCapture {
	public:
		explicit VideoCapture(CaptureSettings settings);

		auto Open(bool perform_warm_up = true) -> bool;
		auto WarmUp() -> bool;
		auto Grab() -> bool;
		auto Read(cv::Mat &frame, cv::Mat *gray_frame = nullptr) -> bool;
		void Release();

		[[nodiscard]] auto IsOpen() const -> bool;
		[[nodiscard]] auto Get(int property) const -> double;
		auto               Set(int property, double value) -> bool;

		[[nodiscard]] auto Error() const -> CaptureError;
		[[nodiscard]] auto ErrorMessage() const -> const std::string &;
		[[nodiscard]] auto Settings() const -> const CaptureSettings &;

	private:
		class FrameReader {
		public:
			virtual ~FrameReader()                    = default;
			virtual auto Read(cv::Mat &frame) -> bool = 0;
		};

		VideoCapture(CaptureSettings settings, std::shared_ptr<FrameReader> frame_reader);

		void SetError(CaptureError error, std::string message);

		friend class VideoCaptureTestAccess;

		CaptureSettings              settings_;
		cv::VideoCapture             capture_;
		std::shared_ptr<FrameReader> frame_reader_;
		CaptureError                 error_ = CaptureError::kNone;
		std::string                  error_message_;
	};

}  // namespace howdy::native
