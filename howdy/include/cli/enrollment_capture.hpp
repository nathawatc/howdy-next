#pragma once

#include "compare/logic.hpp"
#include "config/runtime_config.hpp"
#include "vision/face_detection.hpp"
#include "vision/frame_processing.hpp"
#include "vision/frame_validation.hpp"

#include <cstdint>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

namespace howdy::native {

	struct EnrollmentCaptureResult {
		cv::Mat                    frame;
		std::vector<FaceDetection> faces;
		FaceDetectionStatus        detector_status = FaceDetectionStatus::kOk;
		std::string                detector_error_message;
		int                        valid_frames       = 0;
		int                        dark_tries         = 0;
		int                        black_frames       = 0;
		int                        empty_frames       = 0;
		int                        read_failures      = 0;
		double                     dark_running_total = 0.0;
	};

	enum class EnrollmentCaptureFailure : std::uint8_t {
		kOnlyBlackFrames,
		kOnlyTooDarkFrames,
		kNoSufficientlyBrightFrames,
		kNoUsableFrames,
		kNoFaceDetected,
	};

	[[nodiscard]] inline auto
	classify_enrollment_capture_failure(const EnrollmentCaptureResult &result)
	    -> EnrollmentCaptureFailure {
		if (result.valid_frames == 0 && result.black_frames > 0 && result.empty_frames == 0 &&
		    result.read_failures == 0) {
			return EnrollmentCaptureFailure::kOnlyBlackFrames;
		}
		if (result.valid_frames > 0 && result.valid_frames == result.dark_tries &&
		    result.black_frames == 0 && result.empty_frames == 0 && result.read_failures == 0) {
			return EnrollmentCaptureFailure::kOnlyTooDarkFrames;
		}
		if (result.valid_frames == 0) {
			return EnrollmentCaptureFailure::kNoUsableFrames;
		}
		if (result.valid_frames == result.dark_tries) {
			return EnrollmentCaptureFailure::kNoSufficientlyBrightFrames;
		}
		return EnrollmentCaptureFailure::kNoFaceDetected;
	}

	template <typename Capture, typename FaceModel>
	auto capture_enrollment_sample(Capture &capture, FaceModel &face_model,
	                               const VideoConfig &video_config, int max_frames)
	    -> EnrollmentCaptureResult {
		const float dark_threshold = video_config.dark_threshold;
		auto        clahe          = make_clahe(video_config);

		EnrollmentCaptureResult result;
		cv::Mat                 gray;

		for (int frame_count = 0; frame_count < max_frames; ++frame_count) {
			if (!capture.read(result.frame, &gray)) {
				result.read_failures++;
				continue;
			}

			if (validate_frame(gray, FrameChannelPolicy::kGray) != FrameValidationStatus::kValid) {
				result.empty_frames++;
				continue;
			}

			apply_clahe_if_enabled(gray, video_config, clahe);

			const auto brightness = measure_brightness(gray);
			if (brightness.hist_total == 0.0 || brightness.darkness >= 100.0F) {
				result.black_frames++;
				continue;
			}
			switch (
			    classify_brightness(brightness.hist_total, brightness.darkness, dark_threshold)) {
				case BrightnessDecision::kBlackFrame:
					result.black_frames++;
					continue;
				case BrightnessDecision::kTooDark:
					result.valid_frames++;
					result.dark_running_total += brightness.darkness;
					result.dark_tries++;
					continue;
				case BrightnessDecision::kProcessFrame:
					result.valid_frames++;
					result.dark_running_total += brightness.darkness;
					break;
			}

			auto       prepared         = face_model.prepare_frame(gray);
			const auto detection_result = face_model.detect(prepared);
			if (!detection_result.ok()) {
				result.detector_status        = detection_result.status;
				result.detector_error_message = detection_result.error_message;
				break;
			}
			result.faces = detection_result.detections;
			if (!result.faces.empty()) {
				result.frame = prepared;
				break;
			}
		}

		return result;
	}

}  // namespace howdy::native
