#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
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
	};

	inline constexpr auto kFaceMetricPolicies = std::array{
	    FaceMetricPolicy{.metric                 = FaceMetric::kCosine,
	                     .spelling               = "cosine",
	                     .higher_score_is_better = true,
	                     .threshold_maximum      = 1.0F},
	    FaceMetricPolicy{.metric                 = FaceMetric::kL2,
	                     .spelling               = "l2",
	                     .higher_score_is_better = false,
	                     .threshold_maximum      = 4.0F},
	    FaceMetricPolicy{.metric                 = FaceMetric::kL2Norm,
	                     .spelling               = "l2norm",
	                     .higher_score_is_better = false,
	                     .threshold_maximum      = 4.0F},
	};

	using FaceMetricSpellingList = std::array<std::string_view, kFaceMetricPolicies.size()>;

	inline constexpr auto kFaceMetricSpellings = []() -> FaceMetricSpellingList {
		FaceMetricSpellingList spellings{};
		for (std::size_t index = 0; index < kFaceMetricPolicies.size(); ++index) {
			spellings[index] = kFaceMetricPolicies[index].spelling;
		}
		return spellings;
	}();

	[[nodiscard]] constexpr auto face_metric_threshold_maximum() noexcept -> float {
		float maximum = 0.0F;
		for (const auto &policy : kFaceMetricPolicies) {
			maximum = std::max(maximum, policy.threshold_maximum);
		}
		return maximum;
	}

	[[nodiscard]] constexpr auto face_metric_policy(FaceMetric metric) -> const FaceMetricPolicy * {
		for (const auto &policy : kFaceMetricPolicies) {
			if (policy.metric == metric) {
				return &policy;
			}
		}
		return nullptr;
	}

	[[nodiscard]] constexpr auto face_metric_spelling(FaceMetric metric) -> std::string_view {
		const auto *policy = face_metric_policy(metric);
		return policy == nullptr ? std::string_view{} : policy->spelling;
	}

	[[nodiscard]] inline auto face_metric_equal(std::string_view value, std::string_view spelling)
	    -> bool {
		if (value.size() != spelling.size()) {
			return false;
		}
		for (std::size_t index = 0; index < value.size(); ++index) {
			const auto left  = static_cast<unsigned char>(value[index]);
			const auto right = static_cast<unsigned char>(spelling[index]);
			if (std::tolower(left) != std::tolower(right)) {
				return false;
			}
		}
		return true;
	}

	[[nodiscard]] inline auto parse_face_metric(std::string_view value)
	    -> std::optional<FaceMetric> {
		for (const auto &policy : kFaceMetricPolicies) {
			if (face_metric_equal(value, policy.spelling)) {
				return policy.metric;
			}
		}
		return std::nullopt;
	}

}  // namespace howdy::native
