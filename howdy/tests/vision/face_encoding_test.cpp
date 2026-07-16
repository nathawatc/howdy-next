#include "test_support.hpp"
#include "vision/face_encoding_internal.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace {

	using howdy::test::expect;
	enum class FailureOperation : std::uint8_t {
		kNone,
		kAlign,
		kFeature,
	};

	struct FakeSfaceContext {
		FailureOperation failure = FailureOperation::kNone;
		cv::Mat          aligned = cv::Mat(112, 112, CV_8UC3, cv::Scalar(1, 2, 3));
		cv::Mat feature = cv::Mat(1, static_cast<int>(howdy::native::kSfaceEmbeddingSize), CV_32FC1,
		                          cv::Scalar(0.25F));
	};

	[[noreturn]] void throw_cv_error(const char *operation) {
		throw cv::Exception(cv::Error::StsError, "forced failure", operation, "test", 1);
	}

	void align_face(void *opaque, const howdy::native::FaceAlignmentRequest &request) {
		auto &context = *static_cast<FakeSfaceContext *>(opaque);
		if (context.failure == FailureOperation::kAlign) {
			throw_cv_error("alignCrop");
		}
		request.aligned = context.aligned;
	}

	void extract_feature(void *opaque, [[maybe_unused]] const cv::Mat &aligned, cv::Mat &feature) {
		auto &context = *static_cast<FakeSfaceContext *>(opaque);
		if (context.failure == FailureOperation::kFeature) {
			throw_cv_error("feature");
		}
		feature = context.feature;
	}

	auto encode(FakeSfaceContext &context) -> howdy::native::FaceEncodingResult {
		return howdy::native::encode_sface(cv::Mat(120, 160, CV_8UC3, cv::Scalar(8, 16, 32)), {},
		                                   {
		                                       .context         = &context,
		                                       .align_face      = align_face,
		                                       .extract_feature = extract_feature,
		                                   });
	}

	auto claimed_success(std::size_t size, float value = 0.25F)
	    -> howdy::native::FaceEncodingResult {
		return {
		    .status        = howdy::native::FaceEncodingStatus::kOk,
		    .encoding      = std::vector<float>(size, value),
		    .error_message = {},
		};
	}

	auto expect_encoding_failure(const howdy::native::FaceEncodingResult &result,
	                             const std::string                       &subject) -> bool {
		bool ok = true;
		ok &= expect(!result.ok(), subject + " is not successful");
		ok &= expect(result.encoding.empty(), subject + " exposes no encoding");
		ok &= expect(!result.error_message.empty(), subject + " returns actionable diagnostic");
		return ok;
	}
}  // namespace

auto main() -> int {
	bool ok = true;

	{
		const howdy::native::FaceEncodingResult result;
		ok &= expect(!result.ok(), "default encoding result is not successful");
		ok &= expect(!result.error_message.empty(),
		             "default encoding result has actionable diagnostic");
	}

	{
		const auto result = claimed_success(0);
		ok &= expect(!result.ok(), "claimed success with empty encoding is not successful");
	}

	{
		const auto result = claimed_success(howdy::native::kSfaceEmbeddingSize - 1);
		ok &= expect(!result.ok(), "claimed success with short encoding is not successful");
	}

	{
		const auto result = claimed_success(howdy::native::kSfaceEmbeddingSize + 1);
		ok &= expect(!result.ok(), "claimed success with long encoding is not successful");
	}

	{
		auto result        = claimed_success(howdy::native::kSfaceEmbeddingSize);
		result.encoding[0] = std::numeric_limits<float>::quiet_NaN();
		ok &= expect(!result.ok(), "claimed success with NaN encoding is not successful");
	}

	{
		auto result        = claimed_success(howdy::native::kSfaceEmbeddingSize);
		result.encoding[0] = std::numeric_limits<float>::infinity();
		ok &= expect(!result.ok(), "claimed success with infinite encoding is not successful");
	}

	{
		const auto result = claimed_success(howdy::native::kSfaceEmbeddingSize);
		ok &= expect(result.ok(), "claimed success with valid encoding is successful");
	}

	{
		FakeSfaceContext context{.failure = FailureOperation::kAlign};
		const auto       result = encode(context);
		ok &= expect(result.status == howdy::native::FaceEncodingStatus::kInferenceError,
		             "alignment exception returns inference error");
		ok &= expect(result.error_message == "Face encoding failed during SFace alignment",
		             "alignment exception identifies operation");
	}

	{
		FakeSfaceContext context{.failure = FailureOperation::kFeature};
		const auto       result = encode(context);
		ok &= expect(result.status == howdy::native::FaceEncodingStatus::kInferenceError,
		             "feature exception returns inference error");
		ok &= expect(result.error_message == "Face encoding failed during SFace feature extraction",
		             "feature exception identifies operation");
	}

	{
		FakeSfaceContext context;
		context.aligned   = cv::Mat{};
		const auto result = encode(context);
		ok &= expect_encoding_failure(result, "empty aligned face");
	}

	{
		FakeSfaceContext context;
		const cv::Mat    frame(120, 160, CV_8UC3, cv::Scalar(8, 16, 32));

		const auto missing_context = howdy::native::encode_sface(
		    frame, {},
		    {.context = nullptr, .align_face = align_face, .extract_feature = extract_feature});
		ok &= expect_encoding_failure(missing_context, "missing encoding context");

		const auto missing_align = howdy::native::encode_sface(
		    frame, {},
		    {.context = &context, .align_face = nullptr, .extract_feature = extract_feature});
		ok &= expect_encoding_failure(missing_align, "missing alignment callback");

		const auto missing_feature = howdy::native::encode_sface(
		    frame, {}, {.context = &context, .align_face = align_face, .extract_feature = nullptr});
		ok &= expect_encoding_failure(missing_feature, "missing feature callback");
	}

	{
		FakeSfaceContext context;
		context.feature   = cv::Mat{};
		const auto result = encode(context);
		ok &= expect(result.status == howdy::native::FaceEncodingStatus::kInvalidOutput,
		             "empty feature returns invalid output");
		ok &= expect(result.error_message.contains("empty encoding"),
		             "empty feature returns diagnostic");
	}

	{
		FakeSfaceContext context;
		context.feature   = cv::Mat(1, static_cast<int>(howdy::native::kSfaceEmbeddingSize - 1),
		                            CV_32FC1, cv::Scalar(0.25F));
		const auto result = encode(context);
		ok &= expect(result.status == howdy::native::FaceEncodingStatus::kInvalidOutput,
		             "wrong-size feature returns invalid output");
		ok &= expect(result.error_message == "Face encoding returned invalid embedding",
		             "wrong-size feature returns generic diagnostic");
	}

	{
		FakeSfaceContext context;
		context.feature = cv::Mat(1, static_cast<int>(howdy::native::kSfaceEmbeddingSize), CV_8UC1,
		                          cv::Scalar(1));
		const auto result = encode(context);
		ok &= expect(result.status == howdy::native::FaceEncodingStatus::kInvalidOutput,
		             "wrong-type feature returns invalid output");
		ok &= expect(!result.ok(), "wrong-type feature is not successful");
		ok &= expect(result.error_message == "Face encoding returned invalid embedding",
		             "wrong-type feature returns generic diagnostic");
	}

	{
		FakeSfaceContext context;
		context.feature = cv::Mat(static_cast<int>(howdy::native::kSfaceEmbeddingSize), 1, CV_32FC1,
		                          cv::Scalar(0.25F));
		context.feature.at<float>(0, 0) = 0.5F;
		context.feature.at<float>(static_cast<int>(howdy::native::kSfaceEmbeddingSize - 1), 0) =
		    0.75F;
		const auto result = encode(context);
		ok &= expect(result.ok(), "column embedding succeeds");
		ok &= expect(result.encoding.size() == howdy::native::kSfaceEmbeddingSize &&
		                 result.encoding.front() == 0.5F && result.encoding.back() == 0.75F,
		             "column embedding flattens in element order");
	}

	{
		FakeSfaceContext context;
		context.feature.at<float>(0, 64) = std::numeric_limits<float>::quiet_NaN();
		const auto result                = encode(context);
		ok &= expect(result.status == howdy::native::FaceEncodingStatus::kInvalidOutput,
		             "non-finite feature returns invalid output");
		ok &= expect(result.error_message == "Face encoding returned invalid embedding",
		             "non-finite feature returns generic diagnostic");
	}

	{
		FakeSfaceContext context;
		for (int index = 0; index < context.feature.cols; ++index) {
			context.feature.at<float>(0, index) =
			    static_cast<float>(index) / static_cast<float>(howdy::native::kSfaceEmbeddingSize);
		}
		const auto result = encode(context);
		ok &= expect(result.ok(), "valid feature succeeds");
		ok &= expect(result.encoding.size() == howdy::native::kSfaceEmbeddingSize,
		             "valid feature preserves encoding size");
		ok &= expect(result.error_message.empty(), "valid feature has no failure diagnostic");
		ok &= expect(result.encoding.front() == 0.0F &&
		                 result.encoding.back() ==
		                     127.0F / static_cast<float>(howdy::native::kSfaceEmbeddingSize),
		             "valid feature preserves values and order");
	}

	return ok ? 0 : 1;
}
