#include "cli/add/enrollment_capture.hpp"
#include "config/runtime_config.hpp"
#include "support/frame_limits.hpp"
#include "test_support.hpp"
#include "vision/face_detection.hpp"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/core/hal/interface.h>
#include <opencv2/core/mat.hpp>

namespace {

	using howdy::test::Expect;

	struct FrameRead {
		bool    ok = true;
		cv::Mat frame;
		cv::Mat gray;
	};

	class FakeCapture {
	public:
		explicit FakeCapture(std::vector<FrameRead> reads)
		    : reads_(std::move(reads)) {}

		auto Read(cv::Mat &frame, cv::Mat *gray_frame) -> bool {
			read_calls_++;
			if (next_read_ >= reads_.size()) {
				return false;
			}

			const auto &read = reads_.at(next_read_++);
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

		[[nodiscard]] auto ReadCalls() const -> int {
			return read_calls_;
		}

	private:
		int                    read_calls_ = 0;
		std::vector<FrameRead> reads_;
		std::size_t            next_read_ = 0;
	};

	class FakeFaceModel {
	public:
		auto PrepareFrame(const cv::Mat &frame) -> cv::Mat {
			prepare_calls_++;
			prepared_frames_.push_back(frame.clone());
			return clone_frames_ ? frame.clone() : frame;
		}

		auto Detect([[maybe_unused]] const cv::Mat &frame) -> howdy::native::FaceDetectionResult {
			detect_calls_++;
			if (fail_detection_) {
				return howdy::native::FaceDetectionResult{
				    .status        = howdy::native::FaceDetectionStatus::kInvalidOutput,
				    .error_message = "malformed detector output",
				};
			}
			if (!return_face_ || detect_calls_ <= misses_before_face_) {
				return howdy::native::FaceDetectionResult{};
			}
			return howdy::native::FaceDetectionResult{
			    .detections = {howdy::native::FaceDetection{
			        .box = cv::Rect2f(1.0F, 1.0F, 1.0F, 1.0F),
			    }},
			};
		}

		void SetReturnFace(bool value) {
			return_face_ = value;
		}

		void SetFailDetection(bool value) {
			fail_detection_ = value;
		}

		void SetCloneFrames(bool value) {
			clone_frames_ = value;
		}

		void SetMissesBeforeFace(int value) {
			misses_before_face_ = value;
		}

		[[nodiscard]] auto PrepareCalls() const -> int {
			return prepare_calls_;
		}

		[[nodiscard]] auto DetectCalls() const -> int {
			return detect_calls_;
		}

		[[nodiscard]] auto PreparedFrames() const -> const std::vector<cv::Mat> & {
			return prepared_frames_;
		}

	private:
		bool                 return_face_        = true;
		bool                 fail_detection_     = false;
		bool                 clone_frames_       = true;
		int                  misses_before_face_ = 0;
		int                  prepare_calls_      = 0;
		int                  detect_calls_       = 0;
		std::vector<cv::Mat> prepared_frames_;
	};

	auto TestConfig(bool clahe_enabled = false) -> howdy::native::VideoConfig {
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

	auto BlackRead() -> FrameRead {
		return FrameRead{
		    .ok    = true,
		    .frame = cv::Mat(4, 4, CV_8UC3, cv::Scalar(0, 0, 0)),
		    .gray  = cv::Mat(4, 4, CV_8UC1, cv::Scalar(0)),
		};
	}

	auto ValidRead() -> FrameRead {
		cv::Mat const gray(4, 4, CV_8UC1, cv::Scalar(128));
		gray.row(0).setTo(cv::Scalar(0));
		return FrameRead{
		    .ok    = true,
		    .frame = cv::Mat(4, 4, CV_8UC3, cv::Scalar(128, 128, 128)),
		    .gray  = gray,
		};
	}

	auto TooDarkRead() -> FrameRead {
		cv::Mat const gray(4, 4, CV_8UC1, cv::Scalar(128));
		gray.rowRange(0, 2).setTo(cv::Scalar(0));
		return FrameRead{
		    .ok    = true,
		    .frame = cv::Mat(4, 4, CV_8UC3, cv::Scalar(128, 128, 128)),
		    .gray  = gray,
		};
	}

	auto FailedRead() -> FrameRead {
		return FrameRead{.ok = false};
	}

	auto EmptySuccessfulRead() -> FrameRead {
		return FrameRead{.ok = true};
	}

	auto OversizedGrayRead() -> FrameRead {
		return FrameRead{
		    .ok    = true,
		    .frame = cv::Mat(1, 1, CV_8UC3, cv::Scalar(128, 128, 128)),
		    .gray  = cv::Mat(howdy::native::kMaxFrameDimension + 1, 1, CV_8UC1, cv::Scalar(128)),
		};
	}

	auto UnsupportedGrayChannelRead() -> FrameRead {
		return FrameRead{
		    .ok    = true,
		    .frame = cv::Mat(4, 4, CV_8UC3, cv::Scalar(128, 128, 128)),
		    .gray  = cv::Mat(4, 4, CV_8UC3, cv::Scalar(128, 128, 128)),
		};
	}

	auto BlackFrameIsSkippedWithoutProgress() -> bool {
		FakeCapture   capture({BlackRead()});
		FakeFaceModel face_model;

		const auto result =
		    howdy::native::CaptureEnrollmentSample(capture, face_model, TestConfig(), 1);

		bool ok = true;
		ok &= Expect(result.faces.empty(), "black frame is not accepted");
		ok &= Expect(result.valid_frames == 0, "black frame does not increment accepted progress");
		ok &= Expect(result.dark_tries == 0, "black frame is not counted as dark accepted try");
		ok &= Expect(result.black_frames == 1, "black frame is counted separately");
		return ok;
	}

	auto BlackFrameDoesNotReachFaceModel() -> bool {
		FakeCapture   capture({BlackRead(), BlackRead()});
		FakeFaceModel face_model;

		const auto result =
		    howdy::native::CaptureEnrollmentSample(capture, face_model, TestConfig(), 2);

		bool ok = true;
		ok &= Expect(result.faces.empty(), "black-only capture has no accepted sample to store");
		ok &=
		    Expect(face_model.PrepareCalls() == 0, "black frame does not reach frame preparation");
		ok &= Expect(face_model.DetectCalls() == 0, "black frame does not reach face detection");
		ok &= Expect(howdy::native::ClassifyEnrollmentCaptureFailure(result) ==
		                 howdy::native::EnrollmentCaptureFailure::kOnlyBlackFrames,
		             "black-only capture reports only-black failure");
		return ok;
	}

	auto ValidFrameAfterBlackFramesIsAccepted() -> bool {
		FakeCapture   capture({BlackRead(), BlackRead(), ValidRead()});
		FakeFaceModel face_model;

		const auto result =
		    howdy::native::CaptureEnrollmentSample(capture, face_model, TestConfig(), 3);

		bool ok = true;
		ok &= Expect(result.faces.size() == 1, "valid frame after black frames is accepted");
		ok &= Expect(result.valid_frames == 1, "only valid frame increments accepted progress");
		ok &= Expect(result.black_frames == 2, "leading black frames stay skipped");
		ok &= Expect(face_model.PrepareCalls() == 1, "only valid frame reaches preparation");
		ok &= Expect(face_model.DetectCalls() == 1, "only valid frame reaches detection");
		return ok;
	}

	auto TooDarkFramesIncrementDarkCountersWithoutDetection() -> bool {
		FakeCapture   capture({TooDarkRead(), TooDarkRead()});
		FakeFaceModel face_model;

		const auto result =
		    howdy::native::CaptureEnrollmentSample(capture, face_model, TestConfig(), 2);

		bool ok = true;
		ok &= Expect(result.faces.empty(), "too-dark frames are not accepted");
		ok &= Expect(result.valid_frames == 2, "too-dark frames increment valid frame count");
		ok &= Expect(result.dark_tries == 2, "too-dark frames increment dark try count");
		ok &= Expect(face_model.PrepareCalls() == 0, "too-dark frames do not reach preparation");
		ok &= Expect(face_model.DetectCalls() == 0, "too-dark frames do not reach detection");
		ok &= Expect(howdy::native::ClassifyEnrollmentCaptureFailure(result) ==
		                 howdy::native::EnrollmentCaptureFailure::kOnlyTooDarkFrames,
		             "too-dark-only capture keeps too-dark failure classification");
		return ok;
	}

	auto MixedBlackAndTooDarkFramesReportNoBrightFrames() -> bool {
		FakeCapture   capture({BlackRead(), TooDarkRead()});
		FakeFaceModel face_model;

		const auto result =
		    howdy::native::CaptureEnrollmentSample(capture, face_model, TestConfig(), 2);
		const auto classification = howdy::native::ClassifyEnrollmentCaptureFailure(result);

		bool ok = true;
		ok &= Expect(result.black_frames == 1, "mixed capture counts black frame");
		ok &= Expect(result.valid_frames == 1, "mixed capture counts too-dark valid frame");
		ok &= Expect(result.dark_tries == 1, "mixed capture counts too-dark try");
		ok &= Expect(face_model.DetectCalls() == 0,
		             "mixed black and too-dark frames do not reach detection");
		ok &= Expect(classification != howdy::native::EnrollmentCaptureFailure::kOnlyTooDarkFrames,
		             "mixed black and too-dark frames do not report all-too-dark diagnostic");
		ok &= Expect(classification != howdy::native::EnrollmentCaptureFailure::kNoFaceDetected,
		             "mixed black and too-dark frames do not report no-face diagnostic");
		ok &= Expect(classification ==
		                 howdy::native::EnrollmentCaptureFailure::kNoSufficientlyBrightFrames,
		             "mixed black and too-dark frames report no sufficiently bright frames");
		return ok;
	}

	auto ProcessableFrameWithoutFaceContinuesToLaterFrames() -> bool {
		FakeCapture   capture({ValidRead(), ValidRead()});
		FakeFaceModel face_model;
		face_model.SetMissesBeforeFace(1);

		const auto result =
		    howdy::native::CaptureEnrollmentSample(capture, face_model, TestConfig(), 2);

		bool ok = true;
		ok &= Expect(result.faces.size() == 1, "later processable frame with face is accepted");
		ok &=
		    Expect(result.valid_frames == 2, "processable no-face frame counts and loop continues");
		ok &= Expect(face_model.PrepareCalls() == 2, "both processable frames reach preparation");
		ok &= Expect(face_model.DetectCalls() == 2, "both processable frames reach detection");
		return ok;
	}

	auto ProcessableFramesWithoutFacesReportNoFaceDetected() -> bool {
		FakeCapture   capture({ValidRead(), ValidRead()});
		FakeFaceModel face_model;
		face_model.SetReturnFace(false);

		const auto result =
		    howdy::native::CaptureEnrollmentSample(capture, face_model, TestConfig(), 2);

		bool ok = true;
		ok &= Expect(result.faces.empty(), "processable frames without faces leave faces empty");
		ok &=
		    Expect(face_model.PrepareCalls() == 2, "processable no-face frames reach preparation");
		ok &= Expect(face_model.DetectCalls() == 2, "processable no-face frames reach detection");
		ok &= Expect(result.valid_frames > result.dark_tries,
		             "processable no-face frames pass darkness gate");
		ok &= Expect(howdy::native::ClassifyEnrollmentCaptureFailure(result) ==
		                 howdy::native::EnrollmentCaptureFailure::kNoFaceDetected,
		             "processable no-face frames report no-face failure");
		return ok;
	}

	auto RepeatedBlackFramesTimeoutWithoutAcceptance() -> bool {
		FakeCapture   capture({BlackRead(), BlackRead(), BlackRead()});
		FakeFaceModel face_model;

		const auto result =
		    howdy::native::CaptureEnrollmentSample(capture, face_model, TestConfig(), 3);

		bool ok = true;
		ok &= Expect(result.faces.empty(), "repeated black frames follow no-face failure path");
		ok &= Expect(result.valid_frames == 0,
		             "repeated black frames never become accepted progress");
		ok &= Expect(result.black_frames == 3, "all repeated black frames are skipped");
		ok &= Expect(capture.ReadCalls() == 3, "capture loop consumes configured frame budget");
		return ok;
	}

	auto EmptySuccessfulReadIsSafelySkipped() -> bool {
		FakeCapture   capture({EmptySuccessfulRead()});
		FakeFaceModel face_model;

		const auto result =
		    howdy::native::CaptureEnrollmentSample(capture, face_model, TestConfig(true), 1);

		bool ok = true;
		ok &= Expect(result.faces.empty(), "empty successful read has no accepted sample to store");
		ok &= Expect(result.empty_frames == 1, "empty successful read is counted separately");
		ok &= Expect(result.read_failures == 0, "empty successful read is not a read failure");
		ok &= Expect(result.black_frames == 0, "empty successful read is not classified as black");
		ok &= Expect(result.valid_frames == 0, "empty successful read does not increment progress");
		ok &= Expect(face_model.PrepareCalls() == 0,
		             "empty successful read does not reach preparation");
		ok &=
		    Expect(face_model.DetectCalls() == 0, "empty successful read does not reach detection");
		return ok;
	}

	auto InvalidGrayFrameCountsAsEmptyWithoutModelWork(FrameRead read, const std::string &label)
	    -> bool {
		FakeCapture   capture({std::move(read)});
		FakeFaceModel face_model;

		const auto result =
		    howdy::native::CaptureEnrollmentSample(capture, face_model, TestConfig(), 1);

		bool ok = true;
		ok &= Expect(result.faces.empty(), label + " has no accepted sample to store");
		ok &= Expect(result.empty_frames == 1, label + " is counted as empty frame");
		ok &= Expect(result.black_frames == 0, label + " is not classified as black");
		ok &= Expect(result.valid_frames == 0, label + " does not increment valid frames");
		ok &= Expect(face_model.PrepareCalls() == 0, label + " does not reach preparation");
		ok &= Expect(face_model.DetectCalls() == 0, label + " does not reach detection");
		return ok;
	}

	auto OversizedGrayFrameCountsAsEmptyWithoutModelWork() -> bool {
		return InvalidGrayFrameCountsAsEmptyWithoutModelWork(OversizedGrayRead(),
		                                                     "oversized gray frame");
	}

	auto UnsupportedGrayChannelCountCountsAsEmptyWithoutModelWork() -> bool {
		return InvalidGrayFrameCountsAsEmptyWithoutModelWork(UnsupportedGrayChannelRead(),
		                                                     "unsupported gray channel count");
	}

	auto FrameValidationBoundariesAreAppliedToEnrollmentGrayFrames() -> bool {
		FakeCapture   capture({FrameRead{
		    .ok    = true,
		    .frame = cv::Mat(1, 1, CV_8UC3, cv::Scalar(128, 128, 128)),
		    .gray  = cv::Mat(howdy::native::kMaxFrameDimension, 1, CV_8UC1, cv::Scalar(128)),
		}});
		FakeFaceModel face_model;
		face_model.SetCloneFrames(false);

		const auto accepted =
		    howdy::native::CaptureEnrollmentSample(capture, face_model, TestConfig(), 1);

		bool ok = true;
		ok &= Expect(accepted.empty_frames == 0, "8192x1 gray frame is not rejected");
		ok &= Expect(accepted.valid_frames == 1, "8192x1 gray frame remains valid");
		ok &= Expect(face_model.PrepareCalls() == 1, "8192x1 gray frame reaches preparation");
		ok &= Expect(face_model.DetectCalls() == 1, "8192x1 gray frame reaches detection");

		ok &= InvalidGrayFrameCountsAsEmptyWithoutModelWork(
		    FrameRead{.ok    = true,
			          .frame = cv::Mat(1, 1, CV_8UC3, cv::Scalar(128, 128, 128)),
			          .gray  = cv::Mat(howdy::native::kMaxFrameDimension + 1, 1, CV_8UC1,
			                           cv::Scalar(128))},
		    "8193x1 gray frame");
		ok &= InvalidGrayFrameCountsAsEmptyWithoutModelWork(
		    FrameRead{.ok    = true,
			          .frame = cv::Mat(1, 1, CV_8UC3, cv::Scalar(128, 128, 128)),
			          .gray  = cv::Mat(1, howdy::native::kMaxFrameDimension + 1, CV_8UC1,
			                           cv::Scalar(128))},
		    "1x8193 gray frame");
		return ok;
	}

	auto ReadFailureIsDistinctFromBlackFrameAndEmptyRead() -> bool {
		FakeCapture   capture({FailedRead(), EmptySuccessfulRead(), BlackRead(), ValidRead()});
		FakeFaceModel face_model;

		const auto result =
		    howdy::native::CaptureEnrollmentSample(capture, face_model, TestConfig(), 4);

		bool ok = true;
		ok &= Expect(result.read_failures == 1, "camera read failure is counted separately");
		ok &= Expect(result.empty_frames == 1, "empty successful read is counted separately");
		ok &= Expect(result.black_frames == 1, "read failure and empty read are not black frames");
		ok &= Expect(result.valid_frames == 1, "valid frame after read failure is still accepted");
		ok &= Expect(result.faces.size() == 1, "read failure does not block later valid frame");
		return ok;
	}

	auto AllReadFailuresDoNotReportBlackFrameFailure() -> bool {
		FakeCapture   capture({FailedRead(), FailedRead()});
		FakeFaceModel face_model;

		const auto result =
		    howdy::native::CaptureEnrollmentSample(capture, face_model, TestConfig(), 2);

		bool ok = true;
		ok &= Expect(result.read_failures == 2, "all read failures stay read failures");
		ok &= Expect(result.black_frames == 0, "read failures do not count as black frames");
		ok &= Expect(howdy::native::ClassifyEnrollmentCaptureFailure(result) ==
		                 howdy::native::EnrollmentCaptureFailure::kNoUsableFrames,
		             "all read failures use no-usable-frame failure classification");
		return ok;
	}

	auto AllEmptySuccessfulReadsDoNotReportBlackFrameFailure() -> bool {
		FakeCapture   capture({EmptySuccessfulRead(), EmptySuccessfulRead()});
		FakeFaceModel face_model;

		const auto result =
		    howdy::native::CaptureEnrollmentSample(capture, face_model, TestConfig(), 2);

		bool ok = true;
		ok &= Expect(result.empty_frames == 2, "all empty successful reads stay empty reads");
		ok &= Expect(result.black_frames == 0, "empty reads do not count as black frames");
		ok &= Expect(howdy::native::ClassifyEnrollmentCaptureFailure(result) ==
		                 howdy::native::EnrollmentCaptureFailure::kNoUsableFrames,
		             "all empty reads use no-usable-frame failure classification");
		return ok;
	}

	auto DetectorFailureStopsCaptureAndRemainsDistinct() -> bool {
		FakeCapture   capture({ValidRead(), ValidRead()});
		FakeFaceModel face_model;
		face_model.SetFailDetection(true);

		const auto result =
		    howdy::native::CaptureEnrollmentSample(capture, face_model, TestConfig(), 2);

		bool ok = true;
		ok &= Expect(result.detector_status == howdy::native::FaceDetectionStatus::kInvalidOutput,
		             "detector failure status is retained");
		ok &= Expect(!result.detector_error_message.empty(),
		             "detector failure diagnostic is retained");
		ok &= Expect(result.faces.empty(), "failed detection produces no enrollment faces");
		ok &= Expect(capture.ReadCalls() == 1, "capture stops before consuming a later frame");
		ok &= Expect(face_model.DetectCalls() == 1, "detector is not called after failure");
		return ok;
	}

	auto EnrollmentRotatesEachFrame() -> bool {
		const cv::Mat source = cv::Mat_<unsigned char>({2, 3}, {32, 64, 96, 128, 160, 192});
		cv::Mat       clockwise;
		cv::Mat       counterclockwise;
		cv::Mat       upside_down;
		cv::rotate(source, clockwise, cv::ROTATE_90_CLOCKWISE);
		cv::rotate(source, counterclockwise, cv::ROTATE_90_COUNTERCLOCKWISE);
		cv::rotate(source, upside_down, cv::ROTATE_180);
		const std::vector<std::vector<cv::Mat>> expected{
		    {source, source, source},
		    {counterclockwise, clockwise, source},
		    {clockwise, counterclockwise, clockwise},
		    {upside_down, upside_down, upside_down},
		};

		bool ok = true;
		for (int mode = 0; mode <= 3; ++mode) {
			FakeCapture   capture({{.gray = source}, {.gray = source}, {.gray = source}});
			FakeFaceModel face_model;
			face_model.SetReturnFace(false);
			auto config           = TestConfig();
			config.rotate         = mode;
			config.dark_threshold = 100.0F;
			const auto result =
			    howdy::native::CaptureEnrollmentSample(capture, face_model, config, 3);
			const auto &frames = face_model.PreparedFrames();
			ok &=
			    Expect(result.valid_frames == 3 && frames.size() == 3,
				       "enrollment processes three frames in rotate mode " + std::to_string(mode));
			if (frames.size() != 3) {
				continue;
			}
			for (std::size_t index = 0; index < frames.size(); ++index) {
				const auto &frame = frames.at(index);
				const auto &want  = expected.at(static_cast<std::size_t>(mode)).at(index);
				ok &= Expect(frame.size() == want.size() && cv::countNonZero(frame != want) == 0,
				             "enrollment rotate mode " + std::to_string(mode) + " frame " +
				                 std::to_string(index + 1));
			}
		}

		FakeCapture   capture({FailedRead(), {.gray = source}});
		FakeFaceModel face_model;
		face_model.SetReturnFace(false);
		auto config           = TestConfig();
		config.rotate         = 1;
		config.dark_threshold = 100.0F;
		const auto result = howdy::native::CaptureEnrollmentSample(capture, face_model, config, 2);
		ok &= Expect(result.read_failures == 1 && face_model.PreparedFrames().size() == 1,
		             "failed enrollment read counts toward rotation cadence");
		if (face_model.PreparedFrames().size() == 1) {
			const auto &frame = face_model.PreparedFrames().front();
			ok &= Expect(frame.size() == clockwise.size() &&
			                 cv::countNonZero(frame != clockwise) == 0,
			             "enrollment rotate mode 1 frame 2 rotates clockwise after read failure");
		}
		return ok;
	}

}  // namespace

auto main() -> int {
	bool ok = true;
	ok &= BlackFrameIsSkippedWithoutProgress();
	ok &= BlackFrameDoesNotReachFaceModel();
	ok &= ValidFrameAfterBlackFramesIsAccepted();
	ok &= TooDarkFramesIncrementDarkCountersWithoutDetection();
	ok &= MixedBlackAndTooDarkFramesReportNoBrightFrames();
	ok &= ProcessableFrameWithoutFaceContinuesToLaterFrames();
	ok &= ProcessableFramesWithoutFacesReportNoFaceDetected();
	ok &= RepeatedBlackFramesTimeoutWithoutAcceptance();
	ok &= EmptySuccessfulReadIsSafelySkipped();
	ok &= OversizedGrayFrameCountsAsEmptyWithoutModelWork();
	ok &= UnsupportedGrayChannelCountCountsAsEmptyWithoutModelWork();
	ok &= FrameValidationBoundariesAreAppliedToEnrollmentGrayFrames();
	ok &= ReadFailureIsDistinctFromBlackFrameAndEmptyRead();
	ok &= AllReadFailuresDoNotReportBlackFrameFailure();
	ok &= AllEmptySuccessfulReadsDoNotReportBlackFrameFailure();
	ok &= DetectorFailureStopsCaptureAndRemainsDistinct();
	ok &= EnrollmentRotatesEachFrame();
	return ok ? 0 : 1;
}
