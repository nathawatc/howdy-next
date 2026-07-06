#include "common/compare_engine.hpp"

#include <cmath>
#include <cstddef>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <opencv2/imgproc.hpp>

namespace {
	int callback_calls_without_context = 0;

	auto expect(bool condition, const std::string &message) -> bool {
		if (!condition) {
			std::cerr << "FAIL: " << message << "\n";
			return false;
		}
		return true;
	}

	auto expect_near(double actual, double expected, double tolerance, const std::string &message)
	    -> bool {
		return expect(std::fabs(actual - expected) <= tolerance, message);
	}

	auto expect_matrix_equal(const cv::Mat &actual, const cv::Mat &expected,
	                         const std::string &message) -> bool {
		if (actual.size() != expected.size() || actual.type() != expected.type()) {
			return expect(false, message);
		}
		return expect(cv::countNonZero(actual != expected) == 0, message);
	}

	auto make_video_config(float dark_threshold = 25.0F, float max_height = 100.0F, int rotate = 0,
	                       bool clahe_enabled = false) -> howdy::native::VideoConfig {
		return {
		    .timeout              = 1,
		    .device_path          = "dummy",
		    .warn_no_device       = false,
		    .max_height           = max_height,
		    .frame_width          = 1,
		    .frame_height         = 1,
		    .clahe_enabled        = clahe_enabled,
		    .clahe_clip_limit     = 2.5F,
		    .clahe_tile_grid_size = 4,
		    .dark_threshold       = dark_threshold,
		    .force_mjpeg          = false,
		    .exposure             = -1,
		    .device_fps           = 30,
		    .rotate               = rotate,
		};
	}

	auto make_quarter_dark_frame() -> cv::Mat {
		cv::Mat frame(4, 4, CV_8UC1, cv::Scalar(128));
		frame.row(0).setTo(cv::Scalar(0));
		return frame;
	}

	auto make_encoding(float value) -> std::vector<float> {
		return std::vector<float>(howdy::native::kSfaceEmbeddingSize, value);
	}

	struct FakeInferenceContext {
		int prepare_calls = 0;
		int detect_calls  = 0;
		int encode_calls  = 0;
		int match_calls   = 0;

		cv::Mat                                          prepared_frame;
		howdy::native::FaceDetectionResult               detection_result;
		std::vector<std::vector<float>>                  encoded_results;
		std::vector<howdy::native::FaceEncodingResult>   encoding_results;
		std::vector<howdy::native::FaceMatch>            match_results;
		std::string                                      encoding_error_message;
		std::optional<howdy::native::FaceEncodingResult> forced_encoding_result;

		std::vector<howdy::native::FaceDetection>    encoded_faces;
		std::vector<std::vector<std::vector<float>>> received_known;
		std::vector<std::vector<float>>              received_probes;
		std::size_t                                  next_encoding = 0;
		std::size_t                                  next_match    = 0;
	};

	auto prepare_face_frame(void *opaque, [[maybe_unused]] const cv::Mat &frame) -> cv::Mat {
		if (opaque == nullptr) {
			callback_calls_without_context++;
			return {};
		}
		auto &context = *static_cast<FakeInferenceContext *>(opaque);
		context.prepare_calls++;
		return context.prepared_frame;
	}

	auto detect_faces(void *opaque, [[maybe_unused]] const cv::Mat &frame)
	    -> howdy::native::FaceDetectionResult {
		if (opaque == nullptr) {
			callback_calls_without_context++;
			return {};
		}
		auto &context = *static_cast<FakeInferenceContext *>(opaque);
		context.detect_calls++;
		return context.detection_result;
	}

	auto encode_face(void *opaque, [[maybe_unused]] const cv::Mat &frame,
	                 const howdy::native::FaceDetection &face)
	    -> howdy::native::FaceEncodingResult {
		if (opaque == nullptr) {
			callback_calls_without_context++;
			return {
			    .status        = howdy::native::FaceEncodingStatus::kInferenceError,
			    .error_message = "missing context",
			};
		}
		auto &context = *static_cast<FakeInferenceContext *>(opaque);
		context.encode_calls++;
		context.encoded_faces.push_back(face);
		if (context.forced_encoding_result.has_value()) {
			return *context.forced_encoding_result;
		}
		if (!context.encoding_results.empty()) {
			return context.encoding_results[context.next_encoding++];
		}
		if (!context.encoding_error_message.empty()) {
			return {
			    .status        = howdy::native::FaceEncodingStatus::kInferenceError,
			    .error_message = context.encoding_error_message,
			};
		}
		return {
		    .status        = howdy::native::FaceEncodingStatus::kOk,
		    .encoding      = context.encoded_results[context.next_encoding++],
		    .error_message = {},
		};
	}

	auto find_best_match(void *opaque, const std::vector<std::vector<float>> &known,
	                     const std::vector<float> &probe) -> howdy::native::FaceMatch {
		if (opaque == nullptr) {
			callback_calls_without_context++;
			return {};
		}
		auto &context = *static_cast<FakeInferenceContext *>(opaque);
		context.match_calls++;
		context.received_known.push_back(known);
		context.received_probes.push_back(probe);
		return context.match_results[context.next_match++];
	}

	auto make_inference_dependencies(FakeInferenceContext &context)
	    -> howdy::native::CompareInferenceDependencies {
		return {
		    .context            = &context,
		    .prepare_face_frame = prepare_face_frame,
		    .detect_faces       = detect_faces,
		    .encode_face        = encode_face,
		    .find_best_match    = find_best_match,
		};
	}

	auto make_detection(float x) -> howdy::native::FaceDetection {
		return {
		    .box        = cv::Rect2f(x, 2.0F, 10.0F, 12.0F),
		    .landmarks  = {},
		    .confidence = 0.95F,
		};
	}

	auto same_detection(const howdy::native::FaceDetection &actual,
	                    const howdy::native::FaceDetection &expected) -> bool {
		return actual.box == expected.box && actual.landmarks == expected.landmarks &&
		       actual.confidence == expected.confidence;
	}

}  // namespace

auto main() -> int {
	using howdy::native::CompareFrameStatus;

	bool ok = true;

	{
		howdy::native::CompareEngine engine(make_video_config());
		const auto                   result = engine.process_gray_frame(cv::Mat(), 1);
		ok &= expect(result.status == CompareFrameStatus::kInvalidInput,
		             "empty frame is invalid input");
		ok &= expect(result.error_message == "Camera grayscale frame is empty",
		             "empty frame error is stable");
	}

	{
		howdy::native::CompareEngine engine(make_video_config());
		const auto                   result =
		    engine.process_gray_frame(cv::Mat(2, 2, CV_8UC3, cv::Scalar(32, 64, 96)), 1);
		ok &= expect(result.status == CompareFrameStatus::kInvalidInput,
		             "BGR frame is invalid grayscale input");
		ok &= expect(result.error_message ==
		                 "Camera grayscale frame has unsupported channel count: 3",
		             "BGR frame channel error is stable");
	}

	{
		howdy::native::CompareEngine engine(make_video_config());
		const auto                   result =
		    engine.process_gray_frame(cv::Mat(2, 2, CV_32FC1, cv::Scalar(64.0F)), 1);
		ok &= expect(result.status == CompareFrameStatus::kInvalidInput,
		             "floating-point frame is invalid input");
		ok &= expect(result.error_message == "Camera grayscale frame has unsupported pixel type: " +
		                                         std::to_string(CV_32FC1),
		             "floating-point frame pixel error is stable");
	}

	{
		howdy::native::CompareEngine engine(make_video_config());
		const auto result = engine.process_gray_frame(cv::Mat(4, 4, CV_8UC1, cv::Scalar(0)), 1);
		ok &= expect(result.status == CompareFrameStatus::kBlackFrame,
		             "black frame is classified before preprocessing");
		ok &= expect(result.brightness.hist_total > 0.0,
		             "black frame retains non-zero histogram total");
		ok &= expect_near(result.brightness.darkness, 100.0, 0.0001,
		                  "black frame retains full darkness");
		ok &= expect(result.working_frame.empty(), "black frame returns no working frame");
	}

	{
		howdy::native::CompareEngine engine(make_video_config(24.0F));
		const auto result = engine.process_gray_frame(make_quarter_dark_frame(), 1);
		ok &= expect(result.status == CompareFrameStatus::kTooDark,
		             "frame above dark threshold is too dark");
		ok &= expect_near(result.brightness.hist_total, 16.0, 0.0001,
		                  "too-dark frame retains histogram total");
		ok &= expect_near(result.brightness.darkness, 25.0, 0.0001,
		                  "too-dark frame retains darkness");
		ok &= expect(result.working_frame.empty(), "too-dark frame returns no working frame");
	}

	{
		howdy::native::CompareEngine engine(make_video_config(25.0F));
		const auto result = engine.process_gray_frame(make_quarter_dark_frame(), 1);
		ok &= expect(result.status == CompareFrameStatus::kReady,
		             "frame at dark threshold is processable");
		ok &= expect_near(result.brightness.darkness, 25.0, 0.0001,
		                  "processable frame retains darkness");
		ok &= expect(result.working_frame.rows == 4 && result.working_frame.cols == 4 &&
		                 result.working_frame.type() == CV_8UC1,
		             "processable frame preserves dimensions and type");
	}

	{
		cv::Mat                      source(4, 6, CV_8UC1, cv::Scalar(128));
		howdy::native::CompareEngine engine(make_video_config(25.0F, 2.0F));
		const auto                   result = engine.process_gray_frame(source, 1);
		ok &= expect(result.status == CompareFrameStatus::kReady, "downscaled frame is ready");
		ok &= expect(result.working_frame.rows == 2 && result.working_frame.cols == 3,
		             "downscale uses compare resize scale");
	}

	{
		cv::Mat                      source(2, 3, CV_8UC1, cv::Scalar(128));
		howdy::native::CompareEngine engine(make_video_config(25.0F, 8.0F));
		const auto                   result = engine.process_gray_frame(source, 1);
		ok &= expect(result.status == CompareFrameStatus::kReady, "small frame is ready");
		ok &= expect(result.working_frame.rows == 2 && result.working_frame.cols == 3,
		             "small frame is not upscaled");
	}

	const cv::Mat source = cv::Mat_<uchar>({2, 3}, {32, 64, 96, 128, 160, 192});

	{
		cv::Mat expected;
		cv::rotate(source, expected, cv::ROTATE_90_COUNTERCLOCKWISE);
		howdy::native::CompareEngine engine(make_video_config(100.0F, 100.0F, 1));
		const auto                   result = engine.process_gray_frame(source.clone(), 1);
		ok &= expect(result.status == CompareFrameStatus::kReady, "rotate 1 frame 1 is ready");
		ok &= expect_matrix_equal(result.working_frame, expected,
		                          "rotate 1 frame 1 rotates counterclockwise");
	}

	{
		cv::Mat expected;
		cv::rotate(source, expected, cv::ROTATE_90_CLOCKWISE);
		howdy::native::CompareEngine engine(make_video_config(100.0F, 100.0F, 1));
		const auto                   result = engine.process_gray_frame(source.clone(), 2);
		ok &= expect(result.status == CompareFrameStatus::kReady, "rotate 1 frame 2 is ready");
		ok &= expect_matrix_equal(result.working_frame, expected,
		                          "rotate 1 frame 2 rotates clockwise");
	}

	{
		howdy::native::CompareEngine engine(make_video_config(100.0F, 100.0F, 1));
		const auto                   result = engine.process_gray_frame(source.clone(), 3);
		ok &= expect(result.status == CompareFrameStatus::kReady, "rotate 1 frame 3 is ready");
		ok &=
		    expect_matrix_equal(result.working_frame, source, "rotate 1 frame 3 remains unchanged");
	}

	{
		cv::Mat expected;
		cv::rotate(source, expected, cv::ROTATE_90_CLOCKWISE);
		howdy::native::CompareEngine engine(make_video_config(100.0F, 100.0F, 2));
		const auto                   result = engine.process_gray_frame(source.clone(), 1);
		ok &= expect(result.status == CompareFrameStatus::kReady, "rotate 2 frame 1 is ready");
		ok &= expect_matrix_equal(result.working_frame, expected,
		                          "rotate 2 frame 1 rotates clockwise");
	}

	{
		cv::Mat expected;
		cv::rotate(source, expected, cv::ROTATE_90_COUNTERCLOCKWISE);
		howdy::native::CompareEngine engine(make_video_config(100.0F, 100.0F, 2));
		const auto                   result = engine.process_gray_frame(source.clone(), 2);
		ok &= expect(result.status == CompareFrameStatus::kReady, "rotate 2 frame 2 is ready");
		ok &= expect_matrix_equal(result.working_frame, expected,
		                          "rotate 2 frame 2 rotates counterclockwise");
	}

	{
		const cv::Mat fixture(16, 16, CV_8UC1, cv::Scalar(16));

		howdy::native::CompareEngine disabled_engine(make_video_config(50.0F, 100.0F, 0, false));
		const auto disabled_result = disabled_engine.process_gray_frame(fixture.clone(), 1);
		ok &= expect(disabled_result.status == CompareFrameStatus::kBlackFrame,
		             "dark fixture without CLAHE is black");
		ok &= expect_near(disabled_result.brightness.hist_total, 256.0, 0.0001,
		                  "dark fixture without CLAHE retains histogram total");
		ok &= expect_near(disabled_result.brightness.darkness, 100.0, 0.0001,
		                  "dark fixture without CLAHE reports full darkness");

		howdy::native::CompareEngine enabled_engine(make_video_config(50.0F, 100.0F, 0, true));
		const auto enabled_result = enabled_engine.process_gray_frame(fixture.clone(), 1);
		ok &= expect(enabled_result.status == CompareFrameStatus::kReady,
		             "CLAHE lifts fixture before brightness classification");
		ok &= expect_near(enabled_result.brightness.hist_total, 256.0, 0.0001,
		                  "CLAHE fixture retains histogram total");
		ok &= expect_near(enabled_result.brightness.darkness, 0.0, 0.0001,
		                  "CLAHE fixture reports no darkest-bin pixels");
	}

	const std::vector<std::vector<float>> known = {
	    {1.0F, 2.0F},
	    {3.0F, 4.0F},
	};
	const cv::Mat working_frame(2, 2, CV_8UC1, cv::Scalar(64));

	{
		FakeInferenceContext context;
		auto                 dependencies = make_inference_dependencies(context);
		dependencies.prepare_face_frame   = nullptr;
		howdy::native::CompareEngine engine(make_video_config(), dependencies, known);
		const auto                   result = engine.process_face_frame(working_frame);
		ok &= expect(result.status == howdy::native::CompareInferenceStatus::kInvalidDependencies,
		             "null inference callback is rejected");
		ok &= expect(context.prepare_calls == 0 && context.detect_calls == 0 &&
		                 context.encode_calls == 0 && context.match_calls == 0,
		             "invalid callback bundle invokes no callbacks");
	}

	{
		FakeInferenceContext         context;
		howdy::native::CompareEngine engine(make_video_config());
		const auto                   result = engine.process_face_frame(working_frame);
		ok &= expect(result.status == howdy::native::CompareInferenceStatus::kInvalidDependencies,
		             "one-argument engine has no inference dependencies");
		ok &= expect(context.prepare_calls == 0 && context.detect_calls == 0 &&
		                 context.encode_calls == 0 && context.match_calls == 0,
		             "missing inference dependencies invoke no callbacks");
	}

	{
		FakeInferenceContext context;
		auto                 dependencies = make_inference_dependencies(context);
		dependencies.context              = nullptr;
		callback_calls_without_context    = 0;
		howdy::native::CompareEngine engine(make_video_config(), dependencies, known);
		const auto                   result = engine.process_face_frame(working_frame);
		ok &= expect(result.status == howdy::native::CompareInferenceStatus::kInvalidDependencies,
		             "null inference context is rejected");
		ok &= expect(callback_calls_without_context == 0,
		             "null inference context invokes no callbacks");
		ok &= expect(context.prepare_calls == 0 && context.detect_calls == 0 &&
		                 context.encode_calls == 0 && context.match_calls == 0,
		             "null inference context leaves fake callback counts unchanged");
	}

	{
		FakeInferenceContext context{
		    .prepared_frame = cv::Mat(2, 2, CV_8UC3, cv::Scalar(32, 64, 96)),
		    .detection_result =
		        {
		            .status     = howdy::native::FaceDetectionStatus::kOk,
		            .detections = {make_detection(1.0F)},
		        },
		    .forced_encoding_result = howdy::native::FaceEncodingResult{},
		};
		howdy::native::CompareEngine engine(make_video_config(),
		                                    make_inference_dependencies(context), known);
		const auto                   result = engine.process_face_frame(working_frame);
		ok &= expect(result.status == howdy::native::CompareInferenceStatus::kEncodingFailed,
		             "default empty encoding fails closed");
		ok &= expect(result.error_message == "Face encoding returned no data",
		             "default empty encoding preserves fallback diagnostic");
		ok &= expect(context.encode_calls == 1 && context.match_calls == 0,
		             "default empty encoding stops before matching");
	}

	{
		std::vector<howdy::native::FaceEncodingResult> malformed_results;
		malformed_results.push_back({
		    .status        = howdy::native::FaceEncodingStatus::kOk,
		    .encoding      = std::vector<float>(howdy::native::kSfaceEmbeddingSize - 1, 0.25F),
		    .error_message = {},
		});
		auto non_finite = howdy::native::FaceEncodingResult{
		    .status        = howdy::native::FaceEncodingStatus::kOk,
		    .encoding      = make_encoding(0.25F),
		    .error_message = {},
		};
		non_finite.encoding[0] = std::numeric_limits<float>::quiet_NaN();
		malformed_results.push_back(std::move(non_finite));

		for (const auto &malformed : malformed_results) {
			FakeInferenceContext context{
			    .prepared_frame = cv::Mat(2, 2, CV_8UC3, cv::Scalar(32, 64, 96)),
			    .detection_result =
			        {
			            .status     = howdy::native::FaceDetectionStatus::kOk,
			            .detections = {make_detection(1.0F)},
			        },
			    .forced_encoding_result = malformed,
			};
			howdy::native::CompareEngine engine(make_video_config(),
			                                    make_inference_dependencies(context), known);
			const auto                   result = engine.process_face_frame(working_frame);
			ok &= expect(result.status == howdy::native::CompareInferenceStatus::kEncodingFailed,
			             "malformed claimed-success encoding fails closed");
			ok &= expect(!result.error_message.empty(),
			             "malformed claimed-success encoding returns diagnostic");
			ok &= expect(context.encode_calls == 1 && context.match_calls == 0,
			             "malformed claimed-success encoding stops before matching");
		}
	}

	{
		FakeInferenceContext context{
		    .prepared_frame = cv::Mat(2, 2, CV_8UC3, cv::Scalar(32, 64, 96)),
		    .detection_result =
		        {
		            .status     = howdy::native::FaceDetectionStatus::kOk,
		            .detections = {make_detection(1.0F)},
		        },
		    .encoded_results = {make_encoding(0.1F)},
		    .match_results   = {{.index = 1, .score = 0.9F, .accepted = true}},
		};
		std::vector<std::vector<float>> caller_known = {
		    {5.0F, 6.0F},
		    {7.0F, 8.0F},
		};
		howdy::native::CompareEngine engine(make_video_config(),
		                                    make_inference_dependencies(context), caller_known);
		caller_known.clear();
		const auto result = engine.process_face_frame(working_frame);
		ok &= expect(result.status == howdy::native::CompareInferenceStatus::kMatch,
		             "caller mutation does not invalidate engine-owned encodings");
		ok &= expect(context.received_known.size() == 1 &&
		                 context.received_known[0] ==
		                     std::vector<std::vector<float>>({{5.0F, 6.0F}, {7.0F, 8.0F}}),
		             "matcher receives independent copy of original encodings");
	}

	{
		FakeInferenceContext         context;
		howdy::native::CompareEngine engine(make_video_config(),
		                                    make_inference_dependencies(context), known);
		const auto                   result = engine.process_face_frame(working_frame);
		ok &= expect(result.status == howdy::native::CompareInferenceStatus::kInvalidPreparedFrame,
		             "empty prepared frame is rejected");
		ok &= expect(result.error_message == "Prepared frame for face detection is empty",
		             "empty prepared frame error is stable");
		ok &= expect(context.prepare_calls == 1 && context.detect_calls == 0 &&
		                 context.encode_calls == 0 && context.match_calls == 0,
		             "invalid prepared frame stops inference callbacks");
	}

	{
		FakeInferenceContext context{
		    .prepared_frame = cv::Mat(2, 2, CV_8UC3, cv::Scalar(32, 64, 96)),
		    .detection_result =
		        {
		            .status        = howdy::native::FaceDetectionStatus::kInferenceError,
		            .error_message = "YuNet inference failed: synthetic failure",
		        },
		};
		howdy::native::CompareEngine engine(make_video_config(),
		                                    make_inference_dependencies(context), known);
		const auto                   result = engine.process_face_frame(working_frame);
		ok &= expect(result.status == howdy::native::CompareInferenceStatus::kDetectionFailed,
		             "detection failure is classified");
		ok &= expect(result.error_message == "YuNet inference failed: synthetic failure",
		             "detection failure error is preserved");
		ok &= expect(context.prepare_calls == 1 && context.detect_calls == 1 &&
		                 context.encode_calls == 0 && context.match_calls == 0,
		             "detection failure stops encoding and matching");
	}

	{
		FakeInferenceContext context{
		    .prepared_frame = cv::Mat(2, 2, CV_8UC3, cv::Scalar(32, 64, 96)),
		    .detection_result =
		        {
		            .status     = howdy::native::FaceDetectionStatus::kOk,
		            .detections = {make_detection(1.0F)},
		        },
		    .encoding_error_message = "SFace feature extraction failed: synthetic failure",
		};
		howdy::native::CompareEngine engine(make_video_config(),
		                                    make_inference_dependencies(context), known);
		const auto                   result = engine.process_face_frame(working_frame);
		ok &= expect(result.status == howdy::native::CompareInferenceStatus::kEncodingFailed,
		             "encoding failure aborts inference instead of returning no match");
		ok &= expect(result.error_message == context.encoding_error_message,
		             "encoding failure error is preserved");
		ok &= expect(context.encode_calls == 1 && context.match_calls == 0,
		             "encoding failure stops before matching");
	}

	{
		FakeInferenceContext context{
		    .prepared_frame = cv::Mat(2, 2, CV_8UC3, cv::Scalar(32, 64, 96)),
		    .detection_result =
		        {
		            .status     = howdy::native::FaceDetectionStatus::kOk,
		            .detections = {make_detection(1.0F), make_detection(20.0F)},
		        },
		    .encoding_results =
		        {
		            {
		                .status        = howdy::native::FaceEncodingStatus::kInferenceError,
		                .error_message = "First face encoding failed",
		            },
		            {
		                .status        = howdy::native::FaceEncodingStatus::kOk,
		                .encoding      = make_encoding(0.3F),
		                .error_message = {},
		            },
		        },
		    .match_results = {{.index = 1, .score = 0.9F, .accepted = true}},
		};
		howdy::native::CompareEngine engine(make_video_config(),
		                                    make_inference_dependencies(context), known);
		const auto                   result = engine.process_face_frame(working_frame);
		ok &= expect(result.status == howdy::native::CompareInferenceStatus::kMatch,
		             "failed first encoding does not hide matching second face");
		ok &= expect(result.winning_index == 1, "second face match index is returned");
		ok &= expect(context.encode_calls == 2 && context.match_calls == 1,
		             "failed first encoding advances to second face and matches once");
	}

	{
		FakeInferenceContext context{
		    .prepared_frame = cv::Mat(2, 2, CV_8UC3, cv::Scalar(32, 64, 96)),
		    .detection_result =
		        {
		            .status     = howdy::native::FaceDetectionStatus::kOk,
		            .detections = {make_detection(1.0F), make_detection(20.0F)},
		        },
		    .encoding_results =
		        {
		            {
		                .status        = howdy::native::FaceEncodingStatus::kInferenceError,
		                .error_message = "First face encoding failed",
		            },
		            {
		                .status        = howdy::native::FaceEncodingStatus::kInvalidOutput,
		                .error_message = "Second face encoding failed",
		            },
		        },
		};
		howdy::native::CompareEngine engine(make_video_config(),
		                                    make_inference_dependencies(context), known);
		const auto                   result = engine.process_face_frame(working_frame);
		ok &= expect(result.status == howdy::native::CompareInferenceStatus::kEncodingFailed,
		             "all failed encodings return encoding failure");
		ok &= expect(result.error_message == "First face encoding failed",
		             "all failed encodings preserve first actionable error");
		ok &= expect(context.encode_calls == 2 && context.match_calls == 0,
		             "all failed encodings skip matching");
	}

	{
		FakeInferenceContext context{
		    .prepared_frame   = cv::Mat(2, 2, CV_8UC3, cv::Scalar(32, 64, 96)),
		    .detection_result = {.status = howdy::native::FaceDetectionStatus::kOk},
		};
		howdy::native::CompareEngine engine(make_video_config(),
		                                    make_inference_dependencies(context), known);
		const auto                   result = engine.process_face_frame(working_frame);
		ok &= expect(result.status == howdy::native::CompareInferenceStatus::kNoMatch,
		             "zero detections return no match");
		ok &= expect(context.prepare_calls == 1 && context.detect_calls == 1 &&
		                 context.encode_calls == 0 && context.match_calls == 0,
		             "zero detections skip encoding and matching");
	}

	{
		const std::vector<howdy::native::FaceMatch> invalid_matches = {
		    {.index = -1, .score = 0.9F, .accepted = true},
		    {.index = static_cast<int>(known.size()), .score = 0.9F, .accepted = true},
		    {.index = 0, .score = std::numeric_limits<float>::quiet_NaN(), .accepted = true},
		    {.index = 0, .score = std::numeric_limits<float>::infinity(), .accepted = true},
		    {.index = 0, .score = -std::numeric_limits<float>::infinity(), .accepted = true},
		};

		for (const auto &invalid_match : invalid_matches) {
			FakeInferenceContext context{
			    .prepared_frame = cv::Mat(2, 2, CV_8UC3, cv::Scalar(32, 64, 96)),
			    .detection_result =
			        {
			            .status     = howdy::native::FaceDetectionStatus::kOk,
			            .detections = {make_detection(1.0F), make_detection(20.0F)},
			        },
			    .encoded_results = {make_encoding(0.1F), make_encoding(0.3F)},
			    .match_results   = {invalid_match},
			};
			howdy::native::CompareEngine engine(make_video_config(),
			                                    make_inference_dependencies(context), known);
			const auto                   result = engine.process_face_frame(working_frame);
			ok &=
			    expect(result.status == howdy::native::CompareInferenceStatus::kInvalidMatchResult,
			           "invalid accepted matcher result fails closed");
			ok &= expect(result.error_message == "Face matcher returned invalid match result",
			             "invalid accepted matcher result returns stable diagnostic");
			ok &= expect(result.winning_index == -1,
			             "invalid accepted matcher result exposes no winner");
			ok &= expect(result.winning_score == 0.0F,
			             "invalid accepted matcher result exposes no winning score");
			ok &= expect(context.encode_calls == 1 && context.match_calls == 1,
			             "invalid accepted matcher result stops before later face");
		}
	}

	{
		const auto           first_detection  = make_detection(1.0F);
		const auto           second_detection = make_detection(20.0F);
		FakeInferenceContext context{
		    .prepared_frame = cv::Mat(2, 2, CV_8UC3, cv::Scalar(32, 64, 96)),
		    .detection_result =
		        {
		            .status     = howdy::native::FaceDetectionStatus::kOk,
		            .detections = {first_detection, second_detection},
		        },
		    .encoded_results = {make_encoding(0.1F), make_encoding(0.3F)},
		    .match_results =
		        {
		            {.index = 0, .score = 0.2F, .accepted = false},
		            {.index = 1, .score = 0.9F, .accepted = true},
		        },
		};
		howdy::native::CompareEngine engine(make_video_config(),
		                                    make_inference_dependencies(context), known);
		const auto                   result = engine.process_face_frame(working_frame);
		ok &= expect(result.status == howdy::native::CompareInferenceStatus::kMatch,
		             "accepted second detection returns match");
		ok &= expect(result.winning_index == 1, "second accepted match index is returned");
		ok &= expect_near(result.winning_score, 0.9, 0.0001,
		                  "second accepted match score is returned");
		ok &= expect(context.encode_calls == 2 && context.match_calls == 2,
		             "rejected detection advances to second detection");
		ok &= expect(context.encoded_faces.size() == 2 &&
		                 same_detection(context.encoded_faces[0], first_detection) &&
		                 same_detection(context.encoded_faces[1], second_detection),
		             "detections are encoded in original order");
		ok &= expect(context.received_probes.size() == 2 &&
		                 context.received_probes[1] == make_encoding(0.3F),
		             "second encoding is passed to second match");
	}

	{
		FakeInferenceContext context{
		    .prepared_frame = cv::Mat(2, 2, CV_8UC3, cv::Scalar(32, 64, 96)),
		    .detection_result =
		        {
		            .status     = howdy::native::FaceDetectionStatus::kOk,
		            .detections = {make_detection(1.0F), make_detection(20.0F)},
		        },
		    .encoded_results = {make_encoding(0.1F), make_encoding(0.3F)},
		    .match_results =
		        {
		            {.index = 0, .score = 0.8F, .accepted = true},
		            {.index = 1, .score = 0.9F, .accepted = true},
		        },
		};
		howdy::native::CompareEngine engine(make_video_config(),
		                                    make_inference_dependencies(context), known);
		const auto                   result = engine.process_face_frame(working_frame);
		ok &= expect(result.status == howdy::native::CompareInferenceStatus::kMatch,
		             "first accepted detection returns match");
		ok &= expect(result.winning_index == 0, "first accepted match index is returned");
		ok &= expect_near(result.winning_score, 0.8, 0.0001,
		                  "first accepted match score is returned");
		ok &= expect(context.encode_calls == 1 && context.match_calls == 1,
		             "first accepted detection short-circuits inference");
	}

	{
		FakeInferenceContext context{
		    .prepared_frame = cv::Mat(2, 2, CV_8UC3, cv::Scalar(32, 64, 96)),
		    .detection_result =
		        {
		            .status     = howdy::native::FaceDetectionStatus::kOk,
		            .detections = {make_detection(1.0F), make_detection(20.0F)},
		        },
		    .encoded_results = {make_encoding(0.1F), make_encoding(0.3F)},
		    .match_results =
		        {
		            {.index    = -1,
		             .score    = std::numeric_limits<float>::quiet_NaN(),
		             .accepted = false},
		            {.index    = -1,
		             .score    = std::numeric_limits<float>::infinity(),
		             .accepted = false},
		        },
		};
		howdy::native::CompareEngine engine(make_video_config(),
		                                    make_inference_dependencies(context), known);
		const auto                   result = engine.process_face_frame(working_frame);
		ok &= expect(result.status == howdy::native::CompareInferenceStatus::kNoMatch,
		             "all rejected detections return no match");
		ok &= expect(result.winning_index == -1,
		             "rejected matcher results retain negative index behavior");
		ok &= expect(context.encode_calls == 2 && context.match_calls == 2,
		             "all rejected detections are evaluated");
		ok &= expect(result.error_message.empty(), "all rejected detections return no error");
	}

	return ok ? 0 : 1;
}
