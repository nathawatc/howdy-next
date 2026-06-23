#include "cli/enrollment_capture.hpp"

#include <cstddef>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

	struct FrameRead {
		bool    ok = true;
		cv::Mat frame;
		cv::Mat gray;
	};

	class FakeCapture {
	public:
		explicit FakeCapture(std::vector<FrameRead> reads)
		    : reads_(std::move(reads)) {}

		auto read(cv::Mat &frame, cv::Mat *gray_frame) -> bool {
			read_calls++;
			if (next_read_ >= reads_.size()) {
				return false;
			}

			const auto &read = reads_[next_read_++];
			if (!read.ok) {
				frame.release();
				if (gray_frame != nullptr) {
					gray_frame->release();
				}
				return false;
			}

			frame = read.frame.clone();
			if (gray_frame != nullptr) {
				*gray_frame = read.gray.clone();
			}
			return true;
		}

		int read_calls = 0;

	private:
		std::vector<FrameRead> reads_;
		std::size_t            next_read_ = 0;
	};

	class FakeFaceModel {
	public:
		auto prepare_frame(const cv::Mat &frame) -> cv::Mat {
			prepare_calls++;
			return frame.clone();
		}

		auto detect(const cv::Mat &frame) -> std::vector<cv::Mat> {
			detect_calls++;
			seen_frames.push_back(frame.clone());
			if (!return_face || detect_calls <= misses_before_face) {
				return {};
			}
			return {cv::Mat(1, 15, CV_32F, cv::Scalar(1.0F))};
		}

		bool                 return_face        = true;
		int                  misses_before_face = 0;
		int                  prepare_calls      = 0;
		int                  detect_calls       = 0;
		std::vector<cv::Mat> seen_frames;
	};

	auto expect(bool condition, const std::string &message) -> bool {
		if (!condition) {
			std::cerr << "FAIL: " << message << "\n";
			return false;
		}
		return true;
	}

	auto test_config(bool clahe_enabled = false) -> howdy::native::VideoConfig {
		return howdy::native::VideoConfig{
		    .timeout              = 1,
		    .device_path          = "dummy",
		    .warn_no_device       = false,
		    .max_height           = 1.0F,
		    .frame_width          = 4,
		    .frame_height         = 4,
		    .clahe_enabled        = clahe_enabled,
		    .clahe_clip_limit     = 2.5F,
		    .clahe_tile_grid_size = 4,
		    .dark_threshold       = 30.0F,
		    .force_mjpeg          = false,
		    .exposure             = -1,
		    .device_fps           = 30,
		    .rotate               = 0,
		};
	}

	auto black_read() -> FrameRead {
		return FrameRead{
		    .ok    = true,
		    .frame = cv::Mat(4, 4, CV_8UC3, cv::Scalar(0, 0, 0)),
		    .gray  = cv::Mat(4, 4, CV_8UC1, cv::Scalar(0)),
		};
	}

	auto valid_read() -> FrameRead {
		cv::Mat gray(4, 4, CV_8UC1, cv::Scalar(128));
		gray.row(0).setTo(cv::Scalar(0));
		return FrameRead{
		    .ok    = true,
		    .frame = cv::Mat(4, 4, CV_8UC3, cv::Scalar(128, 128, 128)),
		    .gray  = gray,
		};
	}

	auto too_dark_read() -> FrameRead {
		cv::Mat gray(4, 4, CV_8UC1, cv::Scalar(128));
		gray.rowRange(0, 2).setTo(cv::Scalar(0));
		return FrameRead{
		    .ok    = true,
		    .frame = cv::Mat(4, 4, CV_8UC3, cv::Scalar(128, 128, 128)),
		    .gray  = gray,
		};
	}

	auto failed_read() -> FrameRead {
		return FrameRead{.ok = false};
	}

	auto empty_successful_read() -> FrameRead {
		return FrameRead{.ok = true};
	}

	auto black_frame_is_skipped_without_progress() -> bool {
		FakeCapture   capture({black_read()});
		FakeFaceModel face_model;

		const auto result =
		    howdy::native::capture_enrollment_sample(capture, face_model, test_config(), 1);

		bool ok = true;
		ok &= expect(result.faces.empty(), "black frame is not accepted");
		ok &= expect(result.valid_frames == 0, "black frame does not increment accepted progress");
		ok &= expect(result.dark_tries == 0, "black frame is not counted as dark accepted try");
		ok &= expect(result.black_frames == 1, "black frame is counted separately");
		return ok;
	}

	auto black_frame_does_not_reach_face_model() -> bool {
		FakeCapture   capture({black_read(), black_read()});
		FakeFaceModel face_model;

		const auto result =
		    howdy::native::capture_enrollment_sample(capture, face_model, test_config(), 2);

		bool ok = true;
		ok &= expect(result.faces.empty(), "black-only capture has no accepted sample to store");
		ok &= expect(face_model.prepare_calls == 0, "black frame does not reach frame preparation");
		ok &= expect(face_model.detect_calls == 0, "black frame does not reach face detection");
		ok &= expect(howdy::native::classify_enrollment_capture_failure(result) ==
		                 howdy::native::EnrollmentCaptureFailure::kOnlyBlackFrames,
		             "black-only capture reports only-black failure");
		return ok;
	}

	auto valid_frame_after_black_frames_is_accepted() -> bool {
		FakeCapture   capture({black_read(), black_read(), valid_read()});
		FakeFaceModel face_model;

		const auto result =
		    howdy::native::capture_enrollment_sample(capture, face_model, test_config(), 3);

		bool ok = true;
		ok &= expect(result.faces.size() == 1, "valid frame after black frames is accepted");
		ok &= expect(result.valid_frames == 1, "only valid frame increments accepted progress");
		ok &= expect(result.black_frames == 2, "leading black frames stay skipped");
		ok &= expect(face_model.prepare_calls == 1, "only valid frame reaches preparation");
		ok &= expect(face_model.detect_calls == 1, "only valid frame reaches detection");
		return ok;
	}

	auto too_dark_frames_increment_dark_counters_without_detection() -> bool {
		FakeCapture   capture({too_dark_read(), too_dark_read()});
		FakeFaceModel face_model;

		const auto result =
		    howdy::native::capture_enrollment_sample(capture, face_model, test_config(), 2);

		bool ok = true;
		ok &= expect(result.faces.empty(), "too-dark frames are not accepted");
		ok &= expect(result.valid_frames == 2, "too-dark frames increment valid frame count");
		ok &= expect(result.dark_tries == 2, "too-dark frames increment dark try count");
		ok &= expect(face_model.prepare_calls == 0, "too-dark frames do not reach preparation");
		ok &= expect(face_model.detect_calls == 0, "too-dark frames do not reach detection");
		ok &= expect(howdy::native::classify_enrollment_capture_failure(result) ==
		                 howdy::native::EnrollmentCaptureFailure::kOnlyTooDarkFrames,
		             "too-dark-only capture keeps too-dark failure classification");
		return ok;
	}

	auto mixed_black_and_too_dark_frames_report_no_bright_frames() -> bool {
		FakeCapture   capture({black_read(), too_dark_read()});
		FakeFaceModel face_model;

		const auto result =
		    howdy::native::capture_enrollment_sample(capture, face_model, test_config(), 2);
		const auto classification = howdy::native::classify_enrollment_capture_failure(result);

		bool ok = true;
		ok &= expect(result.black_frames == 1, "mixed capture counts black frame");
		ok &= expect(result.valid_frames == 1, "mixed capture counts too-dark valid frame");
		ok &= expect(result.dark_tries == 1, "mixed capture counts too-dark try");
		ok &= expect(face_model.detect_calls == 0,
		             "mixed black and too-dark frames do not reach detection");
		ok &= expect(classification != howdy::native::EnrollmentCaptureFailure::kOnlyTooDarkFrames,
		             "mixed black and too-dark frames do not report all-too-dark diagnostic");
		ok &= expect(classification != howdy::native::EnrollmentCaptureFailure::kNoFaceDetected,
		             "mixed black and too-dark frames do not report no-face diagnostic");
		ok &= expect(classification ==
		                 howdy::native::EnrollmentCaptureFailure::kNoSufficientlyBrightFrames,
		             "mixed black and too-dark frames report no sufficiently bright frames");
		return ok;
	}

	auto processable_frame_without_face_continues_to_later_frames() -> bool {
		FakeCapture   capture({valid_read(), valid_read()});
		FakeFaceModel face_model;
		face_model.misses_before_face = 1;

		const auto result =
		    howdy::native::capture_enrollment_sample(capture, face_model, test_config(), 2);

		bool ok = true;
		ok &= expect(result.faces.size() == 1, "later processable frame with face is accepted");
		ok &=
		    expect(result.valid_frames == 2, "processable no-face frame counts and loop continues");
		ok &= expect(face_model.prepare_calls == 2, "both processable frames reach preparation");
		ok &= expect(face_model.detect_calls == 2, "both processable frames reach detection");
		return ok;
	}

	auto processable_frames_without_faces_report_no_face_detected() -> bool {
		FakeCapture   capture({valid_read(), valid_read()});
		FakeFaceModel face_model;
		face_model.return_face = false;

		const auto result =
		    howdy::native::capture_enrollment_sample(capture, face_model, test_config(), 2);

		bool ok = true;
		ok &= expect(result.faces.empty(), "processable frames without faces leave faces empty");
		ok &= expect(face_model.prepare_calls == 2, "processable no-face frames reach preparation");
		ok &= expect(face_model.detect_calls == 2, "processable no-face frames reach detection");
		ok &= expect(result.valid_frames > result.dark_tries,
		             "processable no-face frames pass darkness gate");
		ok &= expect(howdy::native::classify_enrollment_capture_failure(result) ==
		                 howdy::native::EnrollmentCaptureFailure::kNoFaceDetected,
		             "processable no-face frames report no-face failure");
		return ok;
	}

	auto repeated_black_frames_timeout_without_acceptance() -> bool {
		FakeCapture   capture({black_read(), black_read(), black_read()});
		FakeFaceModel face_model;

		const auto result =
		    howdy::native::capture_enrollment_sample(capture, face_model, test_config(), 3);

		bool ok = true;
		ok &= expect(result.faces.empty(), "repeated black frames follow no-face failure path");
		ok &= expect(result.valid_frames == 0,
		             "repeated black frames never become accepted progress");
		ok &= expect(result.black_frames == 3, "all repeated black frames are skipped");
		ok &= expect(capture.read_calls == 3, "capture loop consumes configured frame budget");
		return ok;
	}

	auto empty_successful_read_is_safely_skipped() -> bool {
		FakeCapture   capture({empty_successful_read()});
		FakeFaceModel face_model;

		const auto result =
		    howdy::native::capture_enrollment_sample(capture, face_model, test_config(true), 1);

		bool ok = true;
		ok &= expect(result.faces.empty(), "empty successful read has no accepted sample to store");
		ok &= expect(result.empty_frames == 1, "empty successful read is counted separately");
		ok &= expect(result.read_failures == 0, "empty successful read is not a read failure");
		ok &= expect(result.black_frames == 0, "empty successful read is not classified as black");
		ok &= expect(result.valid_frames == 0, "empty successful read does not increment progress");
		ok &= expect(face_model.prepare_calls == 0,
		             "empty successful read does not reach preparation");
		ok &=
		    expect(face_model.detect_calls == 0, "empty successful read does not reach detection");
		return ok;
	}

	auto read_failure_is_distinct_from_black_frame_and_empty_read() -> bool {
		FakeCapture   capture({failed_read(), empty_successful_read(), black_read(), valid_read()});
		FakeFaceModel face_model;

		const auto result =
		    howdy::native::capture_enrollment_sample(capture, face_model, test_config(), 4);

		bool ok = true;
		ok &= expect(result.read_failures == 1, "camera read failure is counted separately");
		ok &= expect(result.empty_frames == 1, "empty successful read is counted separately");
		ok &= expect(result.black_frames == 1, "read failure and empty read are not black frames");
		ok &= expect(result.valid_frames == 1, "valid frame after read failure is still accepted");
		ok &= expect(result.faces.size() == 1, "read failure does not block later valid frame");
		return ok;
	}

	auto all_read_failures_do_not_report_black_frame_failure() -> bool {
		FakeCapture   capture({failed_read(), failed_read()});
		FakeFaceModel face_model;

		const auto result =
		    howdy::native::capture_enrollment_sample(capture, face_model, test_config(), 2);

		bool ok = true;
		ok &= expect(result.read_failures == 2, "all read failures stay read failures");
		ok &= expect(result.black_frames == 0, "read failures do not count as black frames");
		ok &= expect(howdy::native::classify_enrollment_capture_failure(result) ==
		                 howdy::native::EnrollmentCaptureFailure::kNoUsableFrames,
		             "all read failures use no-usable-frame failure classification");
		return ok;
	}

	auto all_empty_successful_reads_do_not_report_black_frame_failure() -> bool {
		FakeCapture   capture({empty_successful_read(), empty_successful_read()});
		FakeFaceModel face_model;

		const auto result =
		    howdy::native::capture_enrollment_sample(capture, face_model, test_config(), 2);

		bool ok = true;
		ok &= expect(result.empty_frames == 2, "all empty successful reads stay empty reads");
		ok &= expect(result.black_frames == 0, "empty reads do not count as black frames");
		ok &= expect(howdy::native::classify_enrollment_capture_failure(result) ==
		                 howdy::native::EnrollmentCaptureFailure::kNoUsableFrames,
		             "all empty reads use no-usable-frame failure classification");
		return ok;
	}

}  // namespace

int main() {
	bool ok = true;
	ok &= black_frame_is_skipped_without_progress();
	ok &= black_frame_does_not_reach_face_model();
	ok &= valid_frame_after_black_frames_is_accepted();
	ok &= too_dark_frames_increment_dark_counters_without_detection();
	ok &= mixed_black_and_too_dark_frames_report_no_bright_frames();
	ok &= processable_frame_without_face_continues_to_later_frames();
	ok &= processable_frames_without_faces_report_no_face_detected();
	ok &= repeated_black_frames_timeout_without_acceptance();
	ok &= empty_successful_read_is_safely_skipped();
	ok &= read_failure_is_distinct_from_black_frame_and_empty_read();
	ok &= all_read_failures_do_not_report_black_frame_failure();
	ok &= all_empty_successful_reads_do_not_report_black_frame_failure();
	return ok ? 0 : 1;
}
