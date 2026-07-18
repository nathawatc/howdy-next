#include "test_support.hpp"
#include "vision/preview_engine.hpp"

#include <chrono>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <opencv2/imgproc.hpp>

namespace {

	using howdy::test::expect;

	struct Context {
		howdy::native::FaceDetectionResult                 detection_result;
		howdy::native::FaceEncodingResult                  encoding_result;
		std::vector<howdy::native::FaceEncodingResult>     encoding_results;
		howdy::native::FaceMatch                           match_result;
		bool                                               invalid_prepared = false;
		std::size_t                                        next_encoding    = 0;
		std::vector<std::chrono::steady_clock::time_point> now_results;
		std::size_t                                        next_now      = 0;
		int                                                prepare_calls = 0;
		int                                                detect_calls  = 0;
		int                                                encode_calls  = 0;
		int                                                match_calls   = 0;
	};

	auto prepare_frame(void *raw_context, const cv::Mat &frame) -> cv::Mat {
		if (raw_context == nullptr) {
			throw std::logic_error("prepare callback received null context");
		}
		auto &context = *static_cast<Context *>(raw_context);
		context.prepare_calls++;
		if (context.invalid_prepared) {
			return {};
		}
		cv::Mat prepared;
		cv::cvtColor(frame, prepared, cv::COLOR_GRAY2BGR);
		return prepared;
	}

	auto detect_faces(void *raw_context, [[maybe_unused]] const cv::Mat &frame)
	    -> howdy::native::FaceDetectionResult {
		if (raw_context == nullptr) {
			throw std::logic_error("detect callback received null context");
		}
		auto &context = *static_cast<Context *>(raw_context);
		context.detect_calls++;
		return context.detection_result;
	}

	auto encode_face(void *raw_context, [[maybe_unused]] const cv::Mat &frame,
	                 [[maybe_unused]] const howdy::native::FaceDetection &detection)
	    -> howdy::native::FaceEncodingResult {
		if (raw_context == nullptr) {
			throw std::logic_error("encode callback received null context");
		}
		auto &context = *static_cast<Context *>(raw_context);
		context.encode_calls++;
		if (!context.encoding_results.empty()) {
			return context.encoding_results[context.next_encoding++];
		}
		return context.encoding_result;
	}

	auto match_face(void                                                   *raw_context,
	                [[maybe_unused]] const std::vector<std::vector<float>> &known,
	                [[maybe_unused]] const std::vector<float> &probe) -> howdy::native::FaceMatch {
		if (raw_context == nullptr) {
			throw std::logic_error("match callback received null context");
		}
		auto &context = *static_cast<Context *>(raw_context);
		context.match_calls++;
		return context.match_result;
	}

	auto now(void *raw_context) -> std::chrono::steady_clock::time_point {
		if (raw_context == nullptr) {
			throw std::logic_error("clock callback received null context");
		}
		auto &context = *static_cast<Context *>(raw_context);
		return context.now_results[context.next_now++];
	}

	auto valid_encoding() -> howdy::native::FaceEncodingResult {
		return {
		    .status        = howdy::native::FaceEncodingStatus::kOk,
		    .encoding      = std::vector<float>(howdy::native::kSfaceEmbeddingSize, 0.25F),
		    .error_message = {},
		};
	}

	auto test_video_config(float dark_threshold = 50.0F) -> howdy::native::VideoConfig {
		howdy::native::VideoConfig config{};
		config.dark_threshold = dark_threshold;
		return config;
	}

	auto make_context() -> Context {
		return {
		    .detection_result = {.status = howdy::native::FaceDetectionStatus::kOk},
		    .encoding_result  = valid_encoding(),
		};
	}

	auto inference_dependencies(Context &context) -> howdy::native::PreviewInferenceDependencies {
		return {
		    .context       = &context,
		    .prepare_frame = prepare_frame,
		    .detect_faces  = detect_faces,
		    .encode_face   = encode_face,
		    .match_face    = match_face,
		};
	}

	auto make_engine(Context &context, float dark_threshold = 99.0F, bool matching_enabled = true)
	    -> howdy::native::PreviewEngine {
		auto config = test_video_config(dark_threshold);
		return howdy::native::PreviewEngine(
		    config, inference_dependencies(context),
		    {std::vector<float>(howdy::native::kSfaceEmbeddingSize, 0.25F)}, 1, matching_enabled);
	}

	auto face() -> howdy::native::FaceDetection {
		return {.box = cv::Rect2f(1.0F, 2.0F, 3.0F, 4.0F), .confidence = 0.9F};
	}

	auto invalid_and_dark_frames_stop_before_inference() -> bool {
		auto       context = make_context();
		auto       engine  = make_engine(context);
		const auto empty   = engine.process_gray_frame({});
		const auto invalid = engine.process_gray_frame(cv::Mat(4, 4, CV_8UC3));
		const auto black   = engine.process_gray_frame(cv::Mat::zeros(8, 8, CV_8UC1));
		bool       ok      = true;
		ok &= expect(empty.status == howdy::native::PreviewFrameStatus::kInvalidFrame,
		             "empty frame is invalid");
		ok &= expect(invalid.status == howdy::native::PreviewFrameStatus::kInvalidFrame,
		             "non-gray frame is invalid");
		ok &= expect(black.status == howdy::native::PreviewFrameStatus::kBlackFrame,
		             "zero frame is black");
		ok &= expect(context.prepare_calls == 0 && context.detect_calls == 0,
		             "invalid and black frames skip inference");
		return ok;
	}

	auto too_dark_frame_is_distinct() -> bool {
		auto    context = make_context();
		auto    engine  = make_engine(context, 40.0F);
		cv::Mat frame(8, 8, CV_8UC1, cv::Scalar(40));
		frame.rowRange(0, 4).setTo(0);
		const auto result = engine.process_gray_frame(std::move(frame));
		return expect(result.status == howdy::native::PreviewFrameStatus::kTooDark,
		              "non-black dark frame is too dark") &&
		       expect(context.detect_calls == 0, "too-dark frame skips detector");
	}

	auto no_face_is_successful_detection() -> bool {
		auto       context = make_context();
		auto       engine  = make_engine(context);
		const auto result  = engine.process_gray_frame(cv::Mat(8, 8, CV_8UC1, cv::Scalar(255)));
		return expect(result.status == howdy::native::PreviewFrameStatus::kNoFace,
		              "zero detections report no face") &&
		       expect(context.detect_calls == 1 && context.encode_calls == 0,
		              "no face stops before encoder");
	}

	auto matching_states_preserve_payload() -> bool {
		auto context                        = make_context();
		context.detection_result.detections = {face()};
		context.match_result                = {.index = 0, .score = 0.812F, .accepted = false};
		auto engine                         = make_engine(context);
		auto result = engine.process_gray_frame(cv::Mat(8, 8, CV_8UC1, cv::Scalar(255)));
		bool ok     = true;
		ok &= expect(result.status == howdy::native::PreviewFrameStatus::kUnmatchedFace,
		             "rejected match reports unmatched face");
		ok &= expect(result.faces.size() == 1 && result.faces[0].matching_attempted &&
		                 result.faces[0].match.score == 0.812F,
		             "unmatched face preserves score");

		context.match_result = {.index = 0, .score = 0.923F, .accepted = true};
		result               = engine.process_gray_frame(cv::Mat(8, 8, CV_8UC1, cv::Scalar(255)));
		ok &= expect(result.status == howdy::native::PreviewFrameStatus::kMatchedFace,
		             "accepted match reports matched face");
		ok &= expect(result.faces[0].match.index == 0 && result.faces[0].match.score == 0.923F,
		             "matched face preserves model index and score for label rendering");
		return ok;
	}

	auto normal_inference_reports_inference_time() -> bool {
		auto context                        = make_context();
		context.detection_result.detections = {face()};
		const auto start                    = std::chrono::steady_clock::time_point{};
		context.now_results                 = {start, start + std::chrono::milliseconds(37)};
		auto dependencies                   = inference_dependencies(context);
		dependencies.now                    = now;
		auto                         config = test_video_config();
		howdy::native::PreviewEngine engine(
		    config, dependencies, {std::vector<float>(howdy::native::kSfaceEmbeddingSize, 0.25F)},
		    1, true);
		const auto result = engine.process_gray_frame(cv::Mat(8, 8, CV_8UC1, cv::Scalar(255)));
		return expect(result.status == howdy::native::PreviewFrameStatus::kUnmatchedFace,
		              "normal inference completes") &&
		       expect(result.inference_time == std::chrono::milliseconds(37),
		              "normal inference reports deterministic inference time") &&
		       expect(context.next_now == 2,
		              "normal inference reads clock at inference boundaries") &&
		       expect(context.prepare_calls == 1 && context.detect_calls == 1 &&
		                  context.encode_calls == 1 && context.match_calls == 1,
		              "inference timing covers full inference path");
	}

	auto detection_failure_does_not_degrade() -> bool {
		auto context             = make_context();
		context.detection_result = {
		    .status        = howdy::native::FaceDetectionStatus::kInferenceError,
		    .error_message = "detector failed",
		};
		auto       engine = make_engine(context);
		const auto result = engine.process_gray_frame(cv::Mat(8, 8, CV_8UC1, cv::Scalar(255)));
		return expect(result.status == howdy::native::PreviewFrameStatus::kDetectionFailed,
		              "detector failure remains explicit") &&
		       expect(result.error_message == "detector failed", "detector diagnostic preserved") &&
		       expect(context.encode_calls == 0 && context.match_calls == 0,
		              "detector failure skips encoder and matcher");
	}

	auto empty_detection_failure_gets_fallback_diagnostic() -> bool {
		auto context             = make_context();
		context.detection_result = {
		    .status        = howdy::native::FaceDetectionStatus::kInferenceError,
		    .error_message = {},
		};
		auto       engine = make_engine(context);
		const auto result = engine.process_gray_frame(cv::Mat(8, 8, CV_8UC1, cv::Scalar(255)));
		return expect(result.status == howdy::native::PreviewFrameStatus::kDetectionFailed,
		              "empty detector failure remains explicit") &&
		       expect(result.error_message == "Face detection failed",
		              "empty detector failure gets stable diagnostic") &&
		       expect(context.encode_calls == 0 && context.match_calls == 0,
		              "empty detector failure skips encoder and matcher");
	}

	auto invalid_prepared_frame_stops_before_detector() -> bool {
		auto context             = make_context();
		context.invalid_prepared = true;
		auto       engine        = make_engine(context);
		const auto result = engine.process_gray_frame(cv::Mat(8, 8, CV_8UC1, cv::Scalar(255)));
		return expect(result.status == howdy::native::PreviewFrameStatus::kDetectionFailed,
		              "invalid prepared frame reports detection failure") &&
		       expect(!result.error_message.empty(), "invalid prepared frame gets diagnostic") &&
		       expect(context.detect_calls == 0 && context.encode_calls == 0 &&
		                  context.match_calls == 0,
		              "invalid prepared frame skips detector, encoder, and matcher");
	}

	auto encoding_failure_does_not_match() -> bool {
		auto context                        = make_context();
		context.detection_result.detections = {face()};
		context.encoding_result             = {
		    .status        = howdy::native::FaceEncodingStatus::kInferenceError,
		    .error_message = "encoder failed",
		};
		auto       engine = make_engine(context);
		const auto result = engine.process_gray_frame(cv::Mat(8, 8, CV_8UC1, cv::Scalar(255)));
		return expect(result.status == howdy::native::PreviewFrameStatus::kEncodingFailed,
		              "encoder failure remains explicit") &&
		       expect(result.error_message == "encoder failed", "encoder diagnostic preserved") &&
		       expect(result.faces.size() == 1 &&
		                  result.faces[0].status ==
		                      howdy::native::PreviewFaceStatus::kEncodingFailed,
		              "encoder failure remains attached to face") &&
		       expect(context.match_calls == 0, "encoder failure skips matcher");
	}

	auto first_face_encoding_failure_preserves_second_match() -> bool {
		auto context                        = make_context();
		context.detection_result.detections = {face(), face()};
		context.match_result                = {.index = 0, .score = 0.91F, .accepted = true};
		context.encoding_results            = {
		    {
		        .status        = howdy::native::FaceEncodingStatus::kInferenceError,
		        .error_message = "first encoder failed",
		    },
		    valid_encoding(),
		};
		auto       engine = make_engine(context);
		const auto result = engine.process_gray_frame(cv::Mat(8, 8, CV_8UC1, cv::Scalar(255)));
		return expect(result.status == howdy::native::PreviewFrameStatus::kMatchedFace,
		              "second face match determines frame status") &&
		       expect(context.encode_calls == 2, "encoding continues after first face failure") &&
		       expect(context.match_calls == 1, "matcher runs for successfully encoded face") &&
		       expect(result.faces.size() == 2 &&
		                  result.faces[0].status ==
		                      howdy::native::PreviewFaceStatus::kEncodingFailed &&
		                  result.faces[1].status == howdy::native::PreviewFaceStatus::kMatched &&
		                  result.faces[1].match.accepted,
		              "failed first face and matched second face remain visible");
	}

	auto all_face_encodings_fail_without_matching() -> bool {
		auto context                        = make_context();
		context.detection_result.detections = {face(), face()};
		context.encoding_results            = {
		    {
		        .status        = howdy::native::FaceEncodingStatus::kInferenceError,
		        .error_message = "first encoder failed",
		    },
		    {
		        .status        = howdy::native::FaceEncodingStatus::kInferenceError,
		        .error_message = "second encoder failed",
		    },
		};
		auto       engine = make_engine(context);
		const auto result = engine.process_gray_frame(cv::Mat(8, 8, CV_8UC1, cv::Scalar(255)));
		return expect(result.status == howdy::native::PreviewFrameStatus::kEncodingFailed,
		              "all encoding failures remain explicit") &&
		       expect(result.error_message == "first encoder failed",
		              "all encoding failures preserve first diagnostic") &&
		       expect(context.encode_calls == 2, "all detected faces attempt encoding") &&
		       expect(context.match_calls == 0, "all encoding failures skip matching") &&
		       expect(result.faces.size() == 2 &&
		                  result.faces[0].status ==
		                      howdy::native::PreviewFaceStatus::kEncodingFailed &&
		                  result.faces[1].status ==
		                      howdy::native::PreviewFaceStatus::kEncodingFailed,
		              "all failed faces retain per-face state");
	}

	auto malformed_success_encoding_does_not_match() -> bool {
		auto context                        = make_context();
		context.detection_result.detections = {face()};
		context.encoding_result             = {
		    .status        = howdy::native::FaceEncodingStatus::kOk,
		    .encoding      = {0.25F},
		    .error_message = {},
		};
		auto       engine = make_engine(context);
		const auto result = engine.process_gray_frame(cv::Mat(8, 8, CV_8UC1, cv::Scalar(255)));
		return expect(result.status == howdy::native::PreviewFrameStatus::kEncodingFailed,
		              "malformed claimed-success encoding fails") &&
		       expect(result.error_message == "Face encoding returned invalid embedding",
		              "malformed encoding gets diagnostic") &&
		       expect(context.match_calls == 0, "malformed encoding skips matcher");
	}

	auto matching_disabled_reports_detected_faces() -> bool {
		auto context                        = make_context();
		context.detection_result.detections = {face()};
		auto                         config = test_video_config();
		howdy::native::PreviewEngine engine(config,
		                                    {
		                                        .context       = &context,
		                                        .prepare_frame = prepare_frame,
		                                        .detect_faces  = detect_faces,
		                                    },
		                                    {}, 0, false);
		const auto result = engine.process_gray_frame(cv::Mat(8, 8, CV_8UC1, cv::Scalar(255)));
		return expect(result.status == howdy::native::PreviewFrameStatus::kFacesDetected,
		              "matching-disabled face gets detected-only status") &&
		       expect(result.faces.size() == 1 && !result.faces[0].matching_attempted,
		              "detected-only face records no matching attempt") &&
		       expect(context.encode_calls == 0 && context.match_calls == 0,
		              "matching-disabled face skips encoder and matcher");
	}

	auto invalid_accepted_match_results_fail_closed() -> bool {
		auto run_invalid_match = [](howdy::native::FaceMatch match,
		                            const std::string       &subject) -> bool {
			auto context                        = make_context();
			context.detection_result.detections = {face()};
			context.match_result                = match;
			auto       engine                   = make_engine(context);
			const auto result = engine.process_gray_frame(cv::Mat(8, 8, CV_8UC1, cv::Scalar(255)));
			return expect(result.status == howdy::native::PreviewFrameStatus::kInvalidMatchResult,
			              subject + " returns invalid match result") &&
			       expect(result.error_message == "Face matcher returned invalid match result",
			              subject + " returns stable diagnostic") &&
			       expect(result.faces.empty(), subject + " exposes no UI-facing model index");
		};

		bool ok = true;
		ok &= run_invalid_match({.index = -1, .score = 0.9F, .accepted = true},
		                        "negative accepted index");
		ok &= run_invalid_match({.index = 1, .score = 0.9F, .accepted = true},
		                        "accepted index at model count");
		ok &= run_invalid_match(
		    {.index = 0, .score = std::numeric_limits<float>::quiet_NaN(), .accepted = true},
		    "accepted NaN score");
		ok &= run_invalid_match(
		    {.index = 0, .score = std::numeric_limits<float>::infinity(), .accepted = true},
		    "accepted infinite score");
		return ok;
	}

	auto invalid_dependency_matrix_fails_closed() -> bool {
		auto run_missing_dependency = [](auto               clear_dependency,
		                                 const std::string &subject) -> auto {
			auto context      = make_context();
			auto dependencies = inference_dependencies(context);
			clear_dependency(dependencies);
			auto                         config = test_video_config();
			howdy::native::PreviewEngine engine(config, dependencies, {}, 0, true);
			const auto result = engine.process_gray_frame(cv::Mat(8, 8, CV_8UC1, cv::Scalar(255)));
			return expect(result.status == howdy::native::PreviewFrameStatus::kInvalidDependencies,
			              subject + " returns invalid dependencies") &&
			       expect(result.error_message ==
			                  "Internal error: missing preview inference dependency",
			              subject + " returns stable diagnostic") &&
			       expect(context.prepare_calls == 0 && context.detect_calls == 0 &&
			                  context.encode_calls == 0 && context.match_calls == 0,
			              subject + " invokes no callback");
		};

		bool ok = true;
		ok &= run_missing_dependency(
		    [](auto &dependencies) -> auto {
			    dependencies.context = nullptr;
		    },
		    "null context");
		ok &= run_missing_dependency(
		    [](auto &dependencies) -> auto {
			    dependencies.prepare_frame = nullptr;
		    },
		    "null prepare callback");
		ok &= run_missing_dependency(
		    [](auto &dependencies) -> auto {
			    dependencies.detect_faces = nullptr;
		    },
		    "null detect callback");
		ok &= run_missing_dependency(
		    [](auto &dependencies) -> auto {
			    dependencies.encode_face = nullptr;
		    },
		    "null encode callback");
		ok &= run_missing_dependency(
		    [](auto &dependencies) -> auto {
			    dependencies.match_face = nullptr;
		    },
		    "null match callback");
		ok &= run_missing_dependency(
		    [](auto &dependencies) -> auto {
			    dependencies.now = nullptr;
		    },
		    "null clock callback");
		return ok;
	}

}  // namespace

auto main() -> int {
	bool ok = true;
	ok &= invalid_and_dark_frames_stop_before_inference();
	ok &= too_dark_frame_is_distinct();
	ok &= no_face_is_successful_detection();
	ok &= matching_states_preserve_payload();
	ok &= normal_inference_reports_inference_time();
	ok &= detection_failure_does_not_degrade();
	ok &= empty_detection_failure_gets_fallback_diagnostic();
	ok &= invalid_prepared_frame_stops_before_detector();
	ok &= encoding_failure_does_not_match();
	ok &= first_face_encoding_failure_preserves_second_match();
	ok &= all_face_encodings_fail_without_matching();
	ok &= malformed_success_encoding_does_not_match();
	ok &= matching_disabled_reports_detected_faces();
	ok &= invalid_accepted_match_results_fail_closed();
	ok &= invalid_dependency_matrix_fails_closed();
	return ok ? 0 : 1;
}
