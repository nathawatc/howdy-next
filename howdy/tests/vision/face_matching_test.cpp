#include "support/face_metric.hpp"
#include "test_support.hpp"
#include "vision/face_matching.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <numbers>
#include <optional>
#include <utility>
#include <vector>

namespace {

	using howdy::native::FaceMetric;
	using howdy::test::Expect;
	using howdy::test::ExpectNear;

	auto PreparedResultsAgree() -> bool & {
		static bool agree = true;
		return agree;
	}

	// Run every semantic case through both calculation paths.
	auto CheckMatch(const std::vector<std::vector<float>> &known, const std::vector<float> &probe,
	                FaceMetric metric, float threshold) -> howdy::native::FaceMatch {
		const auto expected = howdy::native::FindBestFaceMatch(known, probe, metric, threshold);
		const howdy::native::PreparedFaceMatcher matcher(known, metric, threshold);
		const auto                               actual = matcher.Match(probe);
		PreparedResultsAgree() &=
		    Expect(actual.index == expected.index && actual.score == expected.score &&
		               actual.accepted == expected.accepted,
		           "prepared and single-pass matching agree");
		return actual;
	}

}  // namespace

auto main() -> int {
	bool ok = true;

	{
		const auto match = CheckMatch({{1.0F, 2.0F}}, {1.0F, 2.0F}, FaceMetric::kCosine, 0.9F);
		ok &= Expect(match.index == 0, "cosine exact positive match has valid index");
		ok &= ExpectNear(match.score, 1.0F, 1.0e-6F, "cosine exact positive match scores one");
		ok &= Expect(match.accepted, "cosine exact positive match is accepted");
	}

	{
		const auto match = CheckMatch({{0.0F, 1.0F}}, {1.0F, 0.0F}, FaceMetric::kCosine, 0.0F);
		ok &= Expect(match.score == 0.0F, "cosine threshold boundary has exact score");
		ok &= Expect(match.accepted, "cosine threshold boundary is accepted");
	}

	{
		const auto match = CheckMatch({{0.0F, 1.0F}}, {1.0F, 0.0F}, FaceMetric::kCosine, 0.1F);
		ok &= Expect(!match.accepted, "cosine below threshold is rejected");
	}

	{
		const auto match = CheckMatch({{-1.0F}}, {1.0F}, FaceMetric::kCosine, -1.0F);
		ok &= Expect(match.score == -1.0F, "antiparallel cosine match scores negative one");
		ok &= Expect(match.index == 0, "antiparallel cosine match has valid index");
		ok &= Expect(match.accepted, "antiparallel cosine match is accepted at threshold boundary");
	}

	{
		const auto match = CheckMatch({{-1.0F}}, {1.0F}, FaceMetric::kCosine, -0.9F);
		ok &= Expect(!match.accepted, "antiparallel cosine match below threshold is rejected");
	}

	{
		const auto nan = std::numeric_limits<float>::quiet_NaN();
		const auto match =
		    CheckMatch({{nan, 0.0F}, {1.0F, 0.0F}}, {1.0F, 0.0F}, FaceMetric::kCosine, 0.9F);
		ok &= Expect(match.index == 1, "cosine candidate containing NaN is skipped");
		ok &= Expect(match.score == 1.0F, "valid cosine candidate after NaN candidate is scored");
		ok &= Expect(match.accepted, "valid cosine candidate after NaN candidate is accepted");
	}

	{
		const auto infinity = std::numeric_limits<float>::infinity();
		const auto match = CheckMatch({{1.0F, 0.0F}}, {infinity, 0.0F}, FaceMetric::kCosine, -1.0F);
		ok &= Expect(match.index == -1, "non-finite cosine probe keeps sentinel index");
		ok &= Expect(match.score == -1.0F, "non-finite cosine probe keeps sentinel score");
		ok &= Expect(!match.accepted, "non-finite cosine probe is rejected");
	}

	{
		const auto match = CheckMatch({{1.0F, 0.0F}}, {0.0F, 0.0F}, FaceMetric::kCosine, -1.0F);
		ok &= Expect(match.index == -1, "zero-norm cosine probe keeps sentinel index");
		ok &= Expect(match.score == -1.0F, "zero-norm cosine probe keeps sentinel score");
		ok &= Expect(!match.accepted, "zero-norm cosine probe is rejected at negative threshold");
	}

	{
		const auto match = CheckMatch({{0.0F, 0.0F}}, {1.0F, 0.0F}, FaceMetric::kCosine, 0.0F);
		ok &= Expect(match.index == -1, "zero-norm cosine candidate keeps sentinel index");
		ok &= Expect(match.score == -1.0F, "zero-norm cosine candidate keeps sentinel score");
		ok &= Expect(!match.accepted, "zero-norm cosine candidate is rejected at zero threshold");
	}

	{
		const auto match = CheckMatch({{1.0F, 2.0F}}, {1.0F, 2.0F}, FaceMetric::kL2, 0.5F);
		ok &= Expect(match.score == 0.0F, "l2 exact match scores zero");
		ok &= Expect(match.index == 0, "l2 exact match has valid index");
		ok &= Expect(match.accepted, "l2 exact match is accepted");
	}

	// Behavioral regression only: IEEE comparisons can also skip non-finite distance scores.
	{
		const auto               infinity          = std::numeric_limits<float>::infinity();
		const std::vector<float> non_finite_values = {std::numeric_limits<float>::quiet_NaN(),
		                                              infinity, -infinity};
		for (const float non_finite : non_finite_values) {
			const auto match =
			    CheckMatch({{non_finite, 0.0F}, {1.0F, 2.0F}}, {1.0F, 2.0F}, FaceMetric::kL2, 0.5F);
			ok &= Expect(match.index == 1,
			             "valid l2 candidate after non-finite candidate is selected");
			ok &= Expect(match.score == 0.0F,
			             "valid l2 candidate after non-finite candidate scores zero");
			ok &=
			    Expect(match.accepted, "valid l2 candidate after non-finite candidate is accepted");
		}
	}

	{
		const auto match = CheckMatch({{3.0F, 4.0F}}, {0.0F, 0.0F}, FaceMetric::kL2, 5.0F);
		ok &= Expect(match.score == 5.0F, "l2 threshold boundary has exact score");
		ok &= Expect(match.accepted, "l2 threshold boundary is accepted");
	}

	{
		const auto match = CheckMatch({{3.0F, 4.0F}}, {0.0F, 0.0F}, FaceMetric::kL2, 4.9F);
		ok &= Expect(!match.accepted, "l2 above threshold is rejected");
	}

	{
		const auto match = CheckMatch({{1.0F, 2.0F}}, {1.0F, 2.0F}, FaceMetric::kL2Norm, 0.5F);
		ok &= Expect(match.score == 0.0F, "l2norm exact match scores zero");
		ok &= Expect(match.index == 0, "l2norm exact match has valid index");
		ok &= Expect(match.accepted, "l2norm exact match is accepted");
	}

	// Behavioral regression only: IEEE comparisons can also skip non-finite distance scores.
	{
		const auto               infinity          = std::numeric_limits<float>::infinity();
		const std::vector<float> non_finite_values = {std::numeric_limits<float>::quiet_NaN(),
		                                              infinity, -infinity};
		for (const float non_finite : non_finite_values) {
			const auto match = CheckMatch({{1.0F, 2.0F}, {non_finite, 0.0F}}, {1.0F, 2.0F},
			                              FaceMetric::kL2Norm, 0.5F);
			ok &= Expect(match.index == 0,
			             "valid l2norm candidate before non-finite candidate remains selected");
			ok &=
			    Expect(match.score == 0.0F, "l2norm score remains zero after non-finite candidate");
			ok &= Expect(match.accepted,
			             "l2norm exact match before non-finite candidate is accepted");
		}
	}

	{
		const auto               infinity          = std::numeric_limits<float>::infinity();
		const std::vector<float> non_finite_values = {std::numeric_limits<float>::quiet_NaN(),
		                                              infinity, -infinity};
		for (const float non_finite : non_finite_values) {
			const auto match =
			    CheckMatch({{1.0F, 2.0F}}, {non_finite, 2.0F}, FaceMetric::kL2, 0.5F);
			ok &= Expect(match.index == -1, "non-finite l2 probe keeps sentinel index");
			ok &= Expect(match.score == std::numeric_limits<float>::max(),
			             "non-finite l2 probe keeps sentinel score");
			ok &= Expect(!match.accepted, "non-finite l2 probe is rejected");
		}
	}

	{
		const auto               infinity          = std::numeric_limits<float>::infinity();
		const std::vector<float> non_finite_values = {std::numeric_limits<float>::quiet_NaN(),
		                                              infinity, -infinity};
		for (const float non_finite : non_finite_values) {
			const auto match =
			    CheckMatch({{1.0F, 2.0F}}, {non_finite, 2.0F}, FaceMetric::kL2Norm, infinity);
			ok &= Expect(match.index == -1, "non-finite l2norm probe keeps sentinel index");
			ok &= Expect(match.score == std::numeric_limits<float>::max(),
			             "non-finite l2norm probe keeps sentinel score");
			ok &= Expect(!match.accepted,
			             "non-finite l2norm probe is rejected at infinite threshold");
		}
	}

	{
		const auto nan               = std::numeric_limits<float>::quiet_NaN();
		const auto infinity          = std::numeric_limits<float>::infinity();
		const auto negative_infinity = -infinity;
		const auto match = CheckMatch({{nan, infinity}, {negative_infinity, nan}}, {1.0F, 2.0F},
		                              FaceMetric::kL2, infinity);
		ok &= Expect(match.index == -1, "all non-finite l2 candidates keep sentinel index");
		ok &= Expect(match.score == std::numeric_limits<float>::max(),
		             "all non-finite l2 candidates keep sentinel score");
		ok &= Expect(!match.accepted,
		             "all non-finite l2 candidates are rejected at infinite threshold");
	}

	{
		const auto match = CheckMatch({{0.0F, 2.0F}}, {0.0F, 0.0F}, FaceMetric::kL2Norm, 2.0F);
		ok &= Expect(match.score == 2.0F, "l2norm threshold boundary has exact score");
		ok &= Expect(match.accepted, "l2norm threshold boundary is accepted");
	}

	{
		const auto match = CheckMatch({{5.0F, 5.0F}, {1.0F, 1.0F}, {3.0F, 3.0F}}, {0.0F, 0.0F},
		                              FaceMetric::kL2, 2.0F);
		ok &= Expect(match.index == 1, "best candidate is selected across valid candidates");
		ok &= ExpectNear(match.score, std::numbers::sqrt2_v<float>, 1.0e-6F,
		                 "best candidate has lowest distance");
		ok &= Expect(match.accepted, "best candidate within threshold is accepted");
	}

	{
		const auto match = CheckMatch({{0.0F}, {1.0F, 1.0F}}, {1.0F, 1.0F}, FaceMetric::kL2, 0.0F);
		ok &= Expect(match.index == 1, "mismatched-dimension candidate is skipped");
		ok &= Expect(match.score == 0.0F, "valid candidate after mismatch is scored");
		ok &= Expect(match.accepted, "valid candidate after mismatch is accepted");
	}

	{
		const auto match = CheckMatch({{1.0F}}, {1.0F, 2.0F}, FaceMetric::kCosine, 0.0F);
		ok &= Expect(match.index == -1, "all cosine dimension mismatches keep sentinel index");
		ok &= Expect(match.score == -1.0F, "all cosine dimension mismatches keep sentinel score");
		ok &= Expect(!match.accepted, "all cosine dimension mismatches are rejected");
	}

	{
		const auto match = CheckMatch({{1.0F}}, {1.0F, 2.0F}, FaceMetric::kL2, 1.0F);
		ok &= Expect(match.index == -1, "all l2 dimension mismatches keep sentinel index");
		ok &= Expect(match.score == std::numeric_limits<float>::max(),
		             "all l2 dimension mismatches keep sentinel score");
		ok &= Expect(!match.accepted, "all l2 dimension mismatches are rejected");
	}

	{
		const auto match = CheckMatch({}, {1.0F}, FaceMetric::kCosine, 0.0F);
		ok &= Expect(match.index == -1, "empty cosine known list keeps sentinel index");
		ok &= Expect(match.score == -1.0F, "empty cosine known list keeps sentinel score");
		ok &= Expect(!match.accepted, "empty cosine known list is rejected");
	}

	{
		const auto match = CheckMatch({{1.0F}}, {}, FaceMetric::kL2Norm, 1.0F);
		ok &= Expect(match.index == -1, "empty l2norm probe keeps sentinel index");
		ok &= Expect(match.score == std::numeric_limits<float>::max(),
		             "empty l2norm probe keeps sentinel score");
		ok &= Expect(!match.accepted, "empty l2norm probe is rejected");
	}

	{
		const float huge = std::numeric_limits<float>::max();
		for (const auto metric : {FaceMetric::kCosine, FaceMetric::kL2, FaceMetric::kL2Norm}) {
			const auto tied = CheckMatch({{1.0F}, {1.0F}}, {1.0F}, metric, 1.0F);
			ok &= Expect(tied.index == 0, "ties preserve first candidate");
			const auto overflow = CheckMatch({{huge}, {1.0F}}, {1.0F}, metric, 1.0F);
			ok &= Expect(overflow.index == 1, "overflow candidate does not hide finite candidate");
			const auto nan_threshold =
			    CheckMatch({{1.0F}}, {1.0F}, metric, std::numeric_limits<float>::quiet_NaN());
			ok &= Expect(nan_threshold.index == 0 && !nan_threshold.accepted,
			             "NaN threshold rejects winner");
		}
		const auto huge_distance = CheckMatch({{huge}}, {huge}, FaceMetric::kL2, 0.0F);
		ok &= Expect(huge_distance.accepted,
		             "finite L2 candidates do not require finite squared norms");
		const auto tiny = CheckMatch({{1.0e-10F}}, {1.0e-10F}, FaceMetric::kCosine, 0.0F);
		ok &= ExpectNear(tiny.score, 1.0e-8F, 1.0e-14F, "cosine preserves small norm floors");
		const auto underflow = CheckMatch({{1.0e-30F}}, {1.0F}, FaceMetric::kCosine, -1.0F);
		ok &= Expect(underflow.index == -1, "underflowed zero norm is invalid");

		std::vector<std::vector<float>>    candidates{{1.0F, 0.0F}, {0.0F, 1.0F}};
		const auto                        *data = candidates.front().data();
		howdy::native::PreparedFaceMatcher matcher(std::move(candidates), FaceMetric::kCosine,
		                                           0.5F);
		ok &= Expect(matcher.Candidates().front().data() == data,
		             "preparation moves embedding allocations");
		auto                       moved = std::move(matcher);
		const std::array<float, 2> probe{1.0F, 0.0F};
		ok &= Expect(moved.Match(probe).index == 0 && moved.Match(probe).accepted,
		             "moved prepared state accepts array-backed spans");
	}

	ok &= Expect(!howdy::native::ParseFaceMetric("euclidean").has_value(),
	             "unknown face metric is rejected by typed parser");
	{
		// NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
		const auto invalid_metric = static_cast<FaceMetric>(255);
		const auto match          = CheckMatch({{1.0F}}, {1.0F}, invalid_metric, 0.0F);
		ok &= Expect(match.index == -1 && match.score == 0.0F && !match.accepted,
		             "invalid typed metric does not fall through to distance matching");
	}

	{
		const std::vector<std::vector<float>>     candidates{{1.0F, 0.0F}, {0.0F, 1.0F}};
		const std::array<float, 2>                probe{1.0F, 0.0F};
		const std::array<std::optional<float>, 1> short_norms{1.0F};
		const std::array<std::uint8_t, 1>         short_validity{1U};
		const auto                                cosine =
		    howdy::native::face_matching_detail::CosineMatch(candidates, probe, 0.0F, short_norms);
		ok &= Expect(cosine.index == -1 && cosine.score == -1.0F && !cosine.accepted,
		             "cosine rejects mismatched prepared state");
		const auto distance = howdy::native::face_matching_detail::DistanceMatch(
		    candidates, probe, 1.0F, short_validity);
		ok &= Expect(distance.index == -1 && distance.score == std::numeric_limits<float>::max() &&
		                 !distance.accepted,
		             "distance rejects mismatched prepared state");
	}

	{
		float expected_max = 0.0F;
		for (const auto &policy : howdy::native::kFaceMetricPolicies) {
			expected_max = std::max(expected_max, policy.threshold_maximum);
		}
		ok &= Expect(howdy::native::FaceMetricThresholdMaximum() == expected_max,
		             "face_metric_threshold_maximum matches largest registered policy threshold");
	}

	return ok && PreparedResultsAgree() ? 0 : 1;
}
