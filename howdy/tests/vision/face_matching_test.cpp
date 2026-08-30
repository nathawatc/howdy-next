#include "test_support.hpp"
#include "vision/face_matching.hpp"

#include <algorithm>
#include <limits>
#include <numbers>
#include <vector>

namespace {

	using howdy::native::FaceMetric;
	using howdy::test::expect;
	using howdy::test::expect_near;

}  // namespace

auto main() -> int {
	bool ok = true;

	{
		const auto match = howdy::native::find_best_face_match({{1.0F, 2.0F}}, {1.0F, 2.0F},
		                                                       FaceMetric::kCosine, 0.9F);
		ok &= expect(match.index == 0, "cosine exact positive match has valid index");
		ok &= expect_near(match.score, 1.0F, 1.0e-6F, "cosine exact positive match scores one");
		ok &= expect(match.accepted, "cosine exact positive match is accepted");
	}

	{
		const auto match = howdy::native::find_best_face_match({{0.0F, 1.0F}}, {1.0F, 0.0F},
		                                                       FaceMetric::kCosine, 0.0F);
		ok &= expect(match.score == 0.0F, "cosine threshold boundary has exact score");
		ok &= expect(match.accepted, "cosine threshold boundary is accepted");
	}

	{
		const auto match = howdy::native::find_best_face_match({{0.0F, 1.0F}}, {1.0F, 0.0F},
		                                                       FaceMetric::kCosine, 0.1F);
		ok &= expect(!match.accepted, "cosine below threshold is rejected");
	}

	{
		const auto match =
		    howdy::native::find_best_face_match({{-1.0F}}, {1.0F}, FaceMetric::kCosine, -1.0F);
		ok &= expect(match.score == -1.0F, "antiparallel cosine match scores negative one");
		ok &= expect(match.index == 0, "antiparallel cosine match has valid index");
		ok &= expect(match.accepted, "antiparallel cosine match is accepted at threshold boundary");
	}

	{
		const auto match =
		    howdy::native::find_best_face_match({{-1.0F}}, {1.0F}, FaceMetric::kCosine, -0.9F);
		ok &= expect(!match.accepted, "antiparallel cosine match below threshold is rejected");
	}

	{
		const auto nan   = std::numeric_limits<float>::quiet_NaN();
		const auto match = howdy::native::find_best_face_match(
		    {{nan, 0.0F}, {1.0F, 0.0F}}, {1.0F, 0.0F}, FaceMetric::kCosine, 0.9F);
		ok &= expect(match.index == 1, "cosine candidate containing NaN is skipped");
		ok &= expect(match.score == 1.0F, "valid cosine candidate after NaN candidate is scored");
		ok &= expect(match.accepted, "valid cosine candidate after NaN candidate is accepted");
	}

	{
		const auto infinity = std::numeric_limits<float>::infinity();
		const auto match    = howdy::native::find_best_face_match({{1.0F, 0.0F}}, {infinity, 0.0F},
		                                                          FaceMetric::kCosine, -1.0F);
		ok &= expect(match.index == -1, "non-finite cosine probe keeps sentinel index");
		ok &= expect(match.score == -1.0F, "non-finite cosine probe keeps sentinel score");
		ok &= expect(!match.accepted, "non-finite cosine probe is rejected");
	}

	{
		const auto match = howdy::native::find_best_face_match({{1.0F, 0.0F}}, {0.0F, 0.0F},
		                                                       FaceMetric::kCosine, -1.0F);
		ok &= expect(match.index == -1, "zero-norm cosine probe keeps sentinel index");
		ok &= expect(match.score == -1.0F, "zero-norm cosine probe keeps sentinel score");
		ok &= expect(!match.accepted, "zero-norm cosine probe is rejected at negative threshold");
	}

	{
		const auto match = howdy::native::find_best_face_match({{0.0F, 0.0F}}, {1.0F, 0.0F},
		                                                       FaceMetric::kCosine, 0.0F);
		ok &= expect(match.index == -1, "zero-norm cosine candidate keeps sentinel index");
		ok &= expect(match.score == -1.0F, "zero-norm cosine candidate keeps sentinel score");
		ok &= expect(!match.accepted, "zero-norm cosine candidate is rejected at zero threshold");
	}

	{
		const auto match = howdy::native::find_best_face_match({{1.0F, 2.0F}}, {1.0F, 2.0F},
		                                                       FaceMetric::kL2, 0.5F);
		ok &= expect(match.score == 0.0F, "l2 exact match scores zero");
		ok &= expect(match.index == 0, "l2 exact match has valid index");
		ok &= expect(match.accepted, "l2 exact match is accepted");
	}

	// Behavioral regression only: IEEE comparisons can also skip non-finite distance scores.
	{
		const auto               infinity          = std::numeric_limits<float>::infinity();
		const std::vector<float> non_finite_values = {std::numeric_limits<float>::quiet_NaN(),
		                                              infinity, -infinity};
		for (const float non_finite : non_finite_values) {
			const auto match = howdy::native::find_best_face_match(
			    {{non_finite, 0.0F}, {1.0F, 2.0F}}, {1.0F, 2.0F}, FaceMetric::kL2, 0.5F);
			ok &= expect(match.index == 1,
			             "valid l2 candidate after non-finite candidate is selected");
			ok &= expect(match.score == 0.0F,
			             "valid l2 candidate after non-finite candidate scores zero");
			ok &=
			    expect(match.accepted, "valid l2 candidate after non-finite candidate is accepted");
		}
	}

	{
		const auto match = howdy::native::find_best_face_match({{3.0F, 4.0F}}, {0.0F, 0.0F},
		                                                       FaceMetric::kL2, 5.0F);
		ok &= expect(match.score == 5.0F, "l2 threshold boundary has exact score");
		ok &= expect(match.accepted, "l2 threshold boundary is accepted");
	}

	{
		const auto match = howdy::native::find_best_face_match({{3.0F, 4.0F}}, {0.0F, 0.0F},
		                                                       FaceMetric::kL2, 4.9F);
		ok &= expect(!match.accepted, "l2 above threshold is rejected");
	}

	{
		const auto match = howdy::native::find_best_face_match({{1.0F, 2.0F}}, {1.0F, 2.0F},
		                                                       FaceMetric::kL2Norm, 0.5F);
		ok &= expect(match.score == 0.0F, "l2norm exact match scores zero");
		ok &= expect(match.index == 0, "l2norm exact match has valid index");
		ok &= expect(match.accepted, "l2norm exact match is accepted");
	}

	// Behavioral regression only: IEEE comparisons can also skip non-finite distance scores.
	{
		const auto               infinity          = std::numeric_limits<float>::infinity();
		const std::vector<float> non_finite_values = {std::numeric_limits<float>::quiet_NaN(),
		                                              infinity, -infinity};
		for (const float non_finite : non_finite_values) {
			const auto match = howdy::native::find_best_face_match(
			    {{1.0F, 2.0F}, {non_finite, 0.0F}}, {1.0F, 2.0F}, FaceMetric::kL2Norm, 0.5F);
			ok &= expect(match.index == 0,
			             "valid l2norm candidate before non-finite candidate remains selected");
			ok &=
			    expect(match.score == 0.0F, "l2norm score remains zero after non-finite candidate");
			ok &= expect(match.accepted,
			             "l2norm exact match before non-finite candidate is accepted");
		}
	}

	{
		const auto               infinity          = std::numeric_limits<float>::infinity();
		const std::vector<float> non_finite_values = {std::numeric_limits<float>::quiet_NaN(),
		                                              infinity, -infinity};
		for (const float non_finite : non_finite_values) {
			const auto match = howdy::native::find_best_face_match(
			    {{1.0F, 2.0F}}, {non_finite, 2.0F}, FaceMetric::kL2, 0.5F);
			ok &= expect(match.index == -1, "non-finite l2 probe keeps sentinel index");
			ok &= expect(match.score == std::numeric_limits<float>::max(),
			             "non-finite l2 probe keeps sentinel score");
			ok &= expect(!match.accepted, "non-finite l2 probe is rejected");
		}
	}

	{
		const auto               infinity          = std::numeric_limits<float>::infinity();
		const std::vector<float> non_finite_values = {std::numeric_limits<float>::quiet_NaN(),
		                                              infinity, -infinity};
		for (const float non_finite : non_finite_values) {
			const auto match = howdy::native::find_best_face_match(
			    {{1.0F, 2.0F}}, {non_finite, 2.0F}, FaceMetric::kL2Norm, infinity);
			ok &= expect(match.index == -1, "non-finite l2norm probe keeps sentinel index");
			ok &= expect(match.score == std::numeric_limits<float>::max(),
			             "non-finite l2norm probe keeps sentinel score");
			ok &= expect(!match.accepted,
			             "non-finite l2norm probe is rejected at infinite threshold");
		}
	}

	{
		const auto nan               = std::numeric_limits<float>::quiet_NaN();
		const auto infinity          = std::numeric_limits<float>::infinity();
		const auto negative_infinity = -infinity;
		const auto match             = howdy::native::find_best_face_match(
		    {{nan, infinity}, {negative_infinity, nan}}, {1.0F, 2.0F}, FaceMetric::kL2, infinity);
		ok &= expect(match.index == -1, "all non-finite l2 candidates keep sentinel index");
		ok &= expect(match.score == std::numeric_limits<float>::max(),
		             "all non-finite l2 candidates keep sentinel score");
		ok &= expect(!match.accepted,
		             "all non-finite l2 candidates are rejected at infinite threshold");
	}

	{
		const auto match = howdy::native::find_best_face_match({{0.0F, 2.0F}}, {0.0F, 0.0F},
		                                                       FaceMetric::kL2Norm, 2.0F);
		ok &= expect(match.score == 2.0F, "l2norm threshold boundary has exact score");
		ok &= expect(match.accepted, "l2norm threshold boundary is accepted");
	}

	{
		const auto match = howdy::native::find_best_face_match(
		    {{5.0F, 5.0F}, {1.0F, 1.0F}, {3.0F, 3.0F}}, {0.0F, 0.0F}, FaceMetric::kL2, 2.0F);
		ok &= expect(match.index == 1, "best candidate is selected across valid candidates");
		ok &= expect_near(match.score, std::numbers::sqrt2_v<float>, 1.0e-6F,
		                  "best candidate has lowest distance");
		ok &= expect(match.accepted, "best candidate within threshold is accepted");
	}

	{
		const auto match = howdy::native::find_best_face_match({{0.0F}, {1.0F, 1.0F}}, {1.0F, 1.0F},
		                                                       FaceMetric::kL2, 0.0F);
		ok &= expect(match.index == 1, "mismatched-dimension candidate is skipped");
		ok &= expect(match.score == 0.0F, "valid candidate after mismatch is scored");
		ok &= expect(match.accepted, "valid candidate after mismatch is accepted");
	}

	{
		const auto match =
		    howdy::native::find_best_face_match({{1.0F}}, {1.0F, 2.0F}, FaceMetric::kCosine, 0.0F);
		ok &= expect(match.index == -1, "all cosine dimension mismatches keep sentinel index");
		ok &= expect(match.score == -1.0F, "all cosine dimension mismatches keep sentinel score");
		ok &= expect(!match.accepted, "all cosine dimension mismatches are rejected");
	}

	{
		const auto match =
		    howdy::native::find_best_face_match({{1.0F}}, {1.0F, 2.0F}, FaceMetric::kL2, 1.0F);
		ok &= expect(match.index == -1, "all l2 dimension mismatches keep sentinel index");
		ok &= expect(match.score == std::numeric_limits<float>::max(),
		             "all l2 dimension mismatches keep sentinel score");
		ok &= expect(!match.accepted, "all l2 dimension mismatches are rejected");
	}

	{
		const auto match =
		    howdy::native::find_best_face_match({}, {1.0F}, FaceMetric::kCosine, 0.0F);
		ok &= expect(match.index == -1, "empty cosine known list keeps sentinel index");
		ok &= expect(match.score == -1.0F, "empty cosine known list keeps sentinel score");
		ok &= expect(!match.accepted, "empty cosine known list is rejected");
	}

	{
		const auto match =
		    howdy::native::find_best_face_match({{1.0F}}, {}, FaceMetric::kL2Norm, 1.0F);
		ok &= expect(match.index == -1, "empty l2norm probe keeps sentinel index");
		ok &= expect(match.score == std::numeric_limits<float>::max(),
		             "empty l2norm probe keeps sentinel score");
		ok &= expect(!match.accepted, "empty l2norm probe is rejected");
	}

	ok &= expect(!howdy::native::parse_face_metric("euclidean").has_value(),
	             "unknown face metric is rejected by typed parser");
	{
		// NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
		const auto invalid_metric = static_cast<FaceMetric>(255);
		const auto match =
		    howdy::native::find_best_face_match({{1.0F}}, {1.0F}, invalid_metric, 0.0F);
		ok &= expect(match.index == -1 && match.score == 0.0F && !match.accepted,
		             "invalid typed metric does not fall through to distance matching");
	}

	{
		float expected_max = 0.0F;
		for (const auto &policy : howdy::native::kFaceMetricPolicies) {
			expected_max = std::max(expected_max, policy.threshold_maximum);
		}
		ok &= expect(howdy::native::face_metric_threshold_maximum() == expected_max,
		             "face_metric_threshold_maximum matches largest registered policy threshold");
	}

	return ok ? 0 : 1;
}
