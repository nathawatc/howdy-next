#include "compare/compare_engine_test_support.hpp"
#include "compare/engine.hpp"

#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <opencv2/imgproc.hpp>

namespace {

	using howdy::test::Expect;
	using howdy::test::compare_engine::ExpectNear;
	using howdy::test::compare_engine::MakeVideoConfig;
	int callback_calls_without_context = 0;

	auto MakeEncoding(float value) -> std::vector<float> {
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

	auto PrepareFaceFrame(void *opaque, [[maybe_unused]] const cv::Mat &frame) -> cv::Mat {
		if (opaque == nullptr) {
			callback_calls_without_context++;
			return {};
		}
		auto &context = *static_cast<FakeInferenceContext *>(opaque);
		context.prepare_calls++;
		return context.prepared_frame;
	}

	auto DetectFaces(void *opaque, [[maybe_unused]] const cv::Mat &frame)
	    -> howdy::native::FaceDetectionResult {
		if (opaque == nullptr) {
			callback_calls_without_context++;
			return {};
		}
		auto &context = *static_cast<FakeInferenceContext *>(opaque);
		context.detect_calls++;
		return context.detection_result;
	}

	auto EncodeFace(void *opaque, [[maybe_unused]] const cv::Mat &frame,
	                const howdy::native::FaceDetection &face) -> howdy::native::FaceEncodingResult {
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

	auto FindBestMatch(void *opaque, const std::vector<std::vector<float>> &known,
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

	auto MakeInferenceDependencies(FakeInferenceContext &context)
	    -> howdy::native::CompareInferenceDependencies {
		return {
		    .context            = &context,
		    .prepare_face_frame = PrepareFaceFrame,
		    .detect_faces       = DetectFaces,
		    .encode_face        = EncodeFace,
		    .find_best_match    = FindBestMatch,
		};
	}

	auto MakeDetection(float x) -> howdy::native::FaceDetection {
		return {
		    .box        = cv::Rect2f(x, 2.0F, 10.0F, 12.0F),
		    .landmarks  = {},
		    .confidence = 0.95F,
		};
	}

	auto SameDetection(const howdy::native::FaceDetection &actual,
	                   const howdy::native::FaceDetection &expected) -> bool {
		return actual.box == expected.box && actual.landmarks == expected.landmarks &&
		       actual.confidence == expected.confidence;
	}

}  // namespace

auto RunCompareEngineInferenceTests() -> bool {
	bool ok = true;

	const std::vector<std::vector<float>> known = {
	    {1.0F, 2.0F},
	    {3.0F, 4.0F},
	};
	const cv::Mat working_frame(2, 2, CV_8UC1, cv::Scalar(64));

	{
		FakeInferenceContext context;
		auto                 dependencies = MakeInferenceDependencies(context);
		dependencies.prepare_face_frame   = nullptr;
		howdy::native::CompareEngine engine(MakeVideoConfig(), dependencies, known);
		const auto                   result = engine.ProcessFaceFrame(working_frame);
		ok &= Expect(result.status == howdy::native::CompareInferenceStatus::kInvalidDependencies,
		             "null inference callback is rejected");
		ok &= Expect(context.prepare_calls == 0 && context.detect_calls == 0 &&
		                 context.encode_calls == 0 && context.match_calls == 0,
		             "invalid callback bundle invokes no callbacks");
	}

	{
		FakeInferenceContext         context;
		howdy::native::CompareEngine engine(MakeVideoConfig());
		const auto                   result = engine.ProcessFaceFrame(working_frame);
		ok &= Expect(result.status == howdy::native::CompareInferenceStatus::kInvalidDependencies,
		             "one-argument engine has no inference dependencies");
		ok &= Expect(context.prepare_calls == 0 && context.detect_calls == 0 &&
		                 context.encode_calls == 0 && context.match_calls == 0,
		             "missing inference dependencies invoke no callbacks");
	}

	{
		FakeInferenceContext context;
		auto                 dependencies = MakeInferenceDependencies(context);
		dependencies.context              = nullptr;
		callback_calls_without_context    = 0;
		howdy::native::CompareEngine engine(MakeVideoConfig(), dependencies, known);
		const auto                   result = engine.ProcessFaceFrame(working_frame);
		ok &= Expect(result.status == howdy::native::CompareInferenceStatus::kInvalidDependencies,
		             "null inference context is rejected");
		ok &= Expect(callback_calls_without_context == 0,
		             "null inference context invokes no callbacks");
		ok &= Expect(context.prepare_calls == 0 && context.detect_calls == 0 &&
		                 context.encode_calls == 0 && context.match_calls == 0,
		             "null inference context leaves fake callback counts unchanged");
	}

	{
		FakeInferenceContext context{
		    .prepared_frame = cv::Mat(2, 2, CV_8UC3, cv::Scalar(32, 64, 96)),
		    .detection_result =
		        {
		            .status     = howdy::native::FaceDetectionStatus::kOk,
		            .detections = {MakeDetection(1.0F)},
		        },
		    .forced_encoding_result = howdy::native::FaceEncodingResult{},
		};
		howdy::native::CompareEngine engine(MakeVideoConfig(), MakeInferenceDependencies(context),
		                                    known);
		const auto                   result = engine.ProcessFaceFrame(working_frame);
		ok &= Expect(result.status == howdy::native::CompareInferenceStatus::kEncodingFailed,
		             "default empty encoding fails closed");
		ok &= Expect(result.error_message == "Face encoding returned no data",
		             "default empty encoding preserves fallback diagnostic");
		ok &= Expect(context.encode_calls == 1 && context.match_calls == 0,
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
		    .encoding      = MakeEncoding(0.25F),
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
			            .detections = {MakeDetection(1.0F)},
			        },
			    .forced_encoding_result = malformed,
			};
			howdy::native::CompareEngine engine(MakeVideoConfig(),
			                                    MakeInferenceDependencies(context), known);
			const auto                   result = engine.ProcessFaceFrame(working_frame);
			ok &= Expect(result.status == howdy::native::CompareInferenceStatus::kEncodingFailed,
			             "malformed claimed-success encoding fails closed");
			ok &= Expect(!result.error_message.empty(),
			             "malformed claimed-success encoding returns diagnostic");
			ok &= Expect(context.encode_calls == 1 && context.match_calls == 0,
			             "malformed claimed-success encoding stops before matching");
		}
	}

	{
		FakeInferenceContext context{
		    .prepared_frame = cv::Mat(2, 2, CV_8UC3, cv::Scalar(32, 64, 96)),
		    .detection_result =
		        {
		            .status     = howdy::native::FaceDetectionStatus::kOk,
		            .detections = {MakeDetection(1.0F)},
		        },
		    .encoded_results = {MakeEncoding(0.1F)},
		    .match_results   = {{.index = 1, .score = 0.9F, .accepted = true}},
		};
		std::vector<std::vector<float>> caller_known = {
		    {5.0F, 6.0F},
		    {7.0F, 8.0F},
		};
		howdy::native::CompareEngine engine(MakeVideoConfig(), MakeInferenceDependencies(context),
		                                    caller_known);
		caller_known.clear();
		const auto result = engine.ProcessFaceFrame(working_frame);
		ok &= Expect(result.status == howdy::native::CompareInferenceStatus::kMatch,
		             "caller mutation does not invalidate engine-owned encodings");
		ok &= Expect(context.received_known.size() == 1 &&
		                 context.received_known[0] ==
		                     std::vector<std::vector<float>>({{5.0F, 6.0F}, {7.0F, 8.0F}}),
		             "matcher receives independent copy of original encodings");
	}

	{
		FakeInferenceContext         context;
		howdy::native::CompareEngine engine(MakeVideoConfig(), MakeInferenceDependencies(context),
		                                    known);
		const auto                   result = engine.ProcessFaceFrame(working_frame);
		ok &= Expect(result.status == howdy::native::CompareInferenceStatus::kInvalidPreparedFrame,
		             "empty prepared frame is rejected");
		ok &= Expect(result.error_message == "Prepared frame for face detection is empty",
		             "empty prepared frame error is stable");
		ok &= Expect(context.prepare_calls == 1 && context.detect_calls == 0 &&
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
		howdy::native::CompareEngine engine(MakeVideoConfig(), MakeInferenceDependencies(context),
		                                    known);
		const auto                   result = engine.ProcessFaceFrame(working_frame);
		ok &= Expect(result.status == howdy::native::CompareInferenceStatus::kDetectionFailed,
		             "detection failure is classified");
		ok &= Expect(result.error_message == "YuNet inference failed: synthetic failure",
		             "detection failure error is preserved");
		ok &= Expect(context.prepare_calls == 1 && context.detect_calls == 1 &&
		                 context.encode_calls == 0 && context.match_calls == 0,
		             "detection failure stops encoding and matching");
	}

	{
		FakeInferenceContext context{
		    .prepared_frame = cv::Mat(2, 2, CV_8UC3, cv::Scalar(32, 64, 96)),
		    .detection_result =
		        {
		            .status     = howdy::native::FaceDetectionStatus::kOk,
		            .detections = {MakeDetection(1.0F)},
		        },
		    .encoding_error_message = "SFace feature extraction failed: synthetic failure",
		};
		howdy::native::CompareEngine engine(MakeVideoConfig(), MakeInferenceDependencies(context),
		                                    known);
		const auto                   result = engine.ProcessFaceFrame(working_frame);
		ok &= Expect(result.status == howdy::native::CompareInferenceStatus::kEncodingFailed,
		             "encoding failure aborts inference instead of returning no match");
		ok &= Expect(result.error_message == context.encoding_error_message,
		             "encoding failure error is preserved");
		ok &= Expect(context.encode_calls == 1 && context.match_calls == 0,
		             "encoding failure stops before matching");
	}

	{
		FakeInferenceContext context{
		    .prepared_frame = cv::Mat(2, 2, CV_8UC3, cv::Scalar(32, 64, 96)),
		    .detection_result =
		        {
		            .status     = howdy::native::FaceDetectionStatus::kOk,
		            .detections = {MakeDetection(1.0F), MakeDetection(20.0F)},
		        },
		    .encoding_results =
		        {
		            {
		                .status        = howdy::native::FaceEncodingStatus::kInferenceError,
		                .error_message = "First face encoding failed",
		            },
		            {
		                .status        = howdy::native::FaceEncodingStatus::kOk,
		                .encoding      = MakeEncoding(0.3F),
		                .error_message = {},
		            },
		        },
		    .match_results = {{.index = 1, .score = 0.9F, .accepted = true}},
		};
		howdy::native::CompareEngine engine(MakeVideoConfig(), MakeInferenceDependencies(context),
		                                    known);
		const auto                   result = engine.ProcessFaceFrame(working_frame);
		ok &= Expect(result.status == howdy::native::CompareInferenceStatus::kMatch,
		             "failed first encoding does not hide matching second face");
		ok &= Expect(result.winning_index == 1, "second face match index is returned");
		ok &= Expect(context.encode_calls == 2 && context.match_calls == 1,
		             "failed first encoding advances to second face and matches once");
	}

	{
		FakeInferenceContext context{
		    .prepared_frame = cv::Mat(2, 2, CV_8UC3, cv::Scalar(32, 64, 96)),
		    .detection_result =
		        {
		            .status     = howdy::native::FaceDetectionStatus::kOk,
		            .detections = {MakeDetection(1.0F), MakeDetection(20.0F)},
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
		howdy::native::CompareEngine engine(MakeVideoConfig(), MakeInferenceDependencies(context),
		                                    known);
		const auto                   result = engine.ProcessFaceFrame(working_frame);
		ok &= Expect(result.status == howdy::native::CompareInferenceStatus::kEncodingFailed,
		             "all failed encodings return encoding failure");
		ok &= Expect(result.error_message == "First face encoding failed",
		             "all failed encodings preserve first actionable error");
		ok &= Expect(context.encode_calls == 2 && context.match_calls == 0,
		             "all failed encodings skip matching");
	}

	{
		FakeInferenceContext context{
		    .prepared_frame   = cv::Mat(2, 2, CV_8UC3, cv::Scalar(32, 64, 96)),
		    .detection_result = {.status = howdy::native::FaceDetectionStatus::kOk},
		};
		howdy::native::CompareEngine engine(MakeVideoConfig(), MakeInferenceDependencies(context),
		                                    known);
		const auto                   result = engine.ProcessFaceFrame(working_frame);
		ok &= Expect(result.status == howdy::native::CompareInferenceStatus::kNoMatch,
		             "zero detections return no match");
		ok &= Expect(context.prepare_calls == 1 && context.detect_calls == 1 &&
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
			            .detections = {MakeDetection(1.0F), MakeDetection(20.0F)},
			        },
			    .encoded_results = {MakeEncoding(0.1F), MakeEncoding(0.3F)},
			    .match_results   = {invalid_match},
			};
			howdy::native::CompareEngine engine(MakeVideoConfig(),
			                                    MakeInferenceDependencies(context), known);
			const auto                   result = engine.ProcessFaceFrame(working_frame);
			ok &=
			    Expect(result.status == howdy::native::CompareInferenceStatus::kInvalidMatchResult,
			           "invalid accepted matcher result fails closed");
			ok &= Expect(result.error_message == "Face matcher returned invalid match result",
			             "invalid accepted matcher result returns stable diagnostic");
			ok &= Expect(result.winning_index == -1,
			             "invalid accepted matcher result exposes no winner");
			ok &= Expect(result.winning_score == 0.0F,
			             "invalid accepted matcher result exposes no winning score");
			ok &= Expect(context.encode_calls == 1 && context.match_calls == 1,
			             "invalid accepted matcher result stops before later face");
		}
	}

	{
		const auto           first_detection  = MakeDetection(1.0F);
		const auto           second_detection = MakeDetection(20.0F);
		FakeInferenceContext context{
		    .prepared_frame = cv::Mat(2, 2, CV_8UC3, cv::Scalar(32, 64, 96)),
		    .detection_result =
		        {
		            .status     = howdy::native::FaceDetectionStatus::kOk,
		            .detections = {first_detection, second_detection},
		        },
		    .encoded_results = {MakeEncoding(0.1F), MakeEncoding(0.3F)},
		    .match_results =
		        {
		            {.index = 0, .score = 0.2F, .accepted = false},
		            {.index = 1, .score = 0.9F, .accepted = true},
		        },
		};
		howdy::native::CompareEngine engine(MakeVideoConfig(), MakeInferenceDependencies(context),
		                                    known);
		const auto                   result = engine.ProcessFaceFrame(working_frame);
		ok &= Expect(result.status == howdy::native::CompareInferenceStatus::kMatch,
		             "accepted second detection returns match");
		ok &= Expect(result.winning_index == 1, "second accepted match index is returned");
		ok &= ExpectNear(result.winning_score, 0.9, 0.0001,
		                 "second accepted match score is returned");
		ok &= Expect(context.encode_calls == 2 && context.match_calls == 2,
		             "rejected detection advances to second detection");
		ok &= Expect(context.encoded_faces.size() == 2 &&
		                 SameDetection(context.encoded_faces[0], first_detection) &&
		                 SameDetection(context.encoded_faces[1], second_detection),
		             "detections are encoded in original order");
		ok &= Expect(context.received_probes.size() == 2 &&
		                 context.received_probes[1] == MakeEncoding(0.3F),
		             "second encoding is passed to second match");
	}

	{
		FakeInferenceContext context{
		    .prepared_frame = cv::Mat(2, 2, CV_8UC3, cv::Scalar(32, 64, 96)),
		    .detection_result =
		        {
		            .status     = howdy::native::FaceDetectionStatus::kOk,
		            .detections = {MakeDetection(1.0F), MakeDetection(20.0F)},
		        },
		    .encoded_results = {MakeEncoding(0.1F), MakeEncoding(0.3F)},
		    .match_results =
		        {
		            {.index = 0, .score = 0.8F, .accepted = true},
		            {.index = 1, .score = 0.9F, .accepted = true},
		        },
		};
		howdy::native::CompareEngine engine(MakeVideoConfig(), MakeInferenceDependencies(context),
		                                    known);
		const auto                   result = engine.ProcessFaceFrame(working_frame);
		ok &= Expect(result.status == howdy::native::CompareInferenceStatus::kMatch,
		             "first accepted detection returns match");
		ok &= Expect(result.winning_index == 0, "first accepted match index is returned");
		ok &=
		    ExpectNear(result.winning_score, 0.8, 0.0001, "first accepted match score is returned");
		ok &= Expect(context.encode_calls == 1 && context.match_calls == 1,
		             "first accepted detection short-circuits inference");
	}

	{
		FakeInferenceContext context{
		    .prepared_frame = cv::Mat(2, 2, CV_8UC3, cv::Scalar(32, 64, 96)),
		    .detection_result =
		        {
		            .status     = howdy::native::FaceDetectionStatus::kOk,
		            .detections = {MakeDetection(1.0F), MakeDetection(20.0F)},
		        },
		    .encoded_results = {MakeEncoding(0.1F), MakeEncoding(0.3F)},
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
		howdy::native::CompareEngine engine(MakeVideoConfig(), MakeInferenceDependencies(context),
		                                    known);
		const auto                   result = engine.ProcessFaceFrame(working_frame);
		ok &= Expect(result.status == howdy::native::CompareInferenceStatus::kNoMatch,
		             "all rejected detections return no match");
		ok &= Expect(result.winning_index == -1,
		             "rejected matcher results retain negative index behavior");
		ok &= Expect(context.encode_calls == 2 && context.match_calls == 2,
		             "all rejected detections are evaluated");
		ok &= Expect(result.error_message.empty(), "all rejected detections return no error");
	}

	return ok;
}
