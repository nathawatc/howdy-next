#pragma once

#include "support/ascii.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace howdy::native {

	enum class FaceMetric : std::uint8_t {
		kCosine,
		kL2,
		kL2Norm,
	};

	struct FaceMetricPolicy {
		FaceMetric       metric;
		std::string_view spelling;
		bool             higher_score_is_better;
		float            threshold_maximum;
		// Set when a threshold of exactly 0 would already accept unrelated faces.
		bool threshold_minimum_exclusive = false;
	};

	inline constexpr float kFaceMetricThresholdMinimum = 0.0F;

	inline constexpr auto kFaceMetricPolicies = std::array{
	    FaceMetricPolicy{
	        .metric                 = FaceMetric::kCosine,
	        .spelling               = "cosine",
	        .higher_score_is_better = true,
	        .threshold_maximum      = 1.0F,
	        // A score of 0 means no similarity, so a threshold of 0 would accept strangers.
	        .threshold_minimum_exclusive = true,
	    },
	    FaceMetricPolicy{
	        .metric                 = FaceMetric::kL2,
	        .spelling               = "l2",
	        .higher_score_is_better = false,
	        .threshold_maximum      = 4.0F,
	    },
	    // Unit vectors with no similarity (cosine 0) are sqrt(2) apart and opposite ones are 2
	    // apart. The cap is 1.41, just below sqrt(2), so every accepted face has positive
	    // similarity, matching the cosine rule above.
	    FaceMetricPolicy{
	        .metric                 = FaceMetric::kL2Norm,
	        .spelling               = "l2norm",
	        .higher_score_is_better = false,
	        .threshold_maximum      = 1.41F,
	    },
	};

	using FaceMetricSpellingList = std::array<std::string_view, kFaceMetricPolicies.size()>;

	inline constexpr auto kFaceMetricSpellings = [] -> FaceMetricSpellingList {
		FaceMetricSpellingList spellings{};
		std::ranges::transform(kFaceMetricPolicies, spellings.begin(),
		                       [](const FaceMetricPolicy &policy) -> std::string_view {
			                       return policy.spelling;
		                       });
		return spellings;
	}();

	[[nodiscard]] constexpr auto FaceMetricThresholdMaximum() noexcept -> float {
		float maximum = 0.0F;
		for (const auto &policy : kFaceMetricPolicies) {
			maximum = std::max(maximum, policy.threshold_maximum);
		}
		return maximum;
	}

	// False for NaN, so an unparsable threshold never passes.
	[[nodiscard]] constexpr auto FaceMetricThresholdAllowed(const FaceMetricPolicy &policy,
	                                                        float threshold) noexcept -> bool {
		const bool above_minimum = policy.threshold_minimum_exclusive
		                               ? threshold > kFaceMetricThresholdMinimum
		                               : threshold >= kFaceMetricThresholdMinimum;
		return above_minimum && threshold <= policy.threshold_maximum;
	}

	[[nodiscard]] constexpr auto GetFaceMetricPolicy(FaceMetric metric)
	    -> const FaceMetricPolicy * {
		for (const auto &policy : kFaceMetricPolicies) {
			if (policy.metric == metric) {
				return &policy;
			}
		}
		return nullptr;
	}

	[[nodiscard]] constexpr auto FaceMetricSpelling(FaceMetric metric) -> std::string_view {
		const auto *policy = GetFaceMetricPolicy(metric);
		return policy == nullptr ? std::string_view{} : policy->spelling;
	}

	[[nodiscard]] constexpr auto FaceMetricEqual(std::string_view value, std::string_view spelling)
	    -> bool {
		return AsciiEqualIgnoreCase(value, spelling);
	}

	[[nodiscard]] constexpr auto ParseFaceMetric(std::string_view value)
	    -> std::optional<FaceMetric> {
		for (const auto &policy : kFaceMetricPolicies) {
			if (FaceMetricEqual(value, policy.spelling)) {
				return policy.metric;
			}
		}
		return std::nullopt;
	}

}  // namespace howdy::native
