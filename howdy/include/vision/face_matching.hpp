#pragma once

#include "vision/face_metric.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <vector>

namespace howdy::native {
	inline constexpr auto kFaceMatcherInvalidResultMessage =
	    "Face matcher returned invalid match result";

	struct FaceMatch {
		int   index    = -1;
		float score    = 0.0F;
		bool  accepted = false;
	};

	namespace face_matching_detail {
		[[nodiscard]] inline auto VectorNorm(const std::vector<float> &values)
		    -> std::optional<float> {
			float squared_norm = 0.0F;
			for (float value : values) {
				if (!std::isfinite(value)) {
					return std::nullopt;
				}
				squared_norm += value * value;
			}
			if (squared_norm == 0.0F || !std::isfinite(squared_norm)) {
				return std::nullopt;
			}
			const float norm = std::sqrt(std::max(squared_norm, 1.0e-12F));
			return std::isfinite(norm) ? std::optional<float>{norm} : std::nullopt;
		}

		[[nodiscard]] inline auto CosineScore(const std::vector<float> &candidate,
		                                      const std::vector<float> &probe, float probe_norm)
		    -> std::optional<float> {
			if (candidate.size() != probe.size()) {
				return std::nullopt;
			}
			float dot = 0.0F;
			for (std::size_t element = 0; element < probe.size(); ++element) {
				if (!std::isfinite(candidate[element])) {
					return std::nullopt;
				}
				dot += candidate[element] * probe[element];
			}
			const auto candidate_norm = VectorNorm(candidate);
			if (!candidate_norm.has_value() || !std::isfinite(dot)) {
				return std::nullopt;
			}
			const float score = dot / std::max(*candidate_norm * probe_norm, 1.0e-12F);
			return std::isfinite(score) ? std::optional<float>{score} : std::nullopt;
		}

		[[nodiscard]] inline auto CosineMatch(const std::vector<std::vector<float>> &known,
		                                      const std::vector<float> &probe, float threshold)
		    -> FaceMatch {
			const auto probe_norm = VectorNorm(probe);
			if (!probe_norm.has_value()) {
				return {.score = -1.0F};
			}
			FaceMatch match{.score = -1.0F};
			for (std::size_t index = 0; index < known.size(); ++index) {
				const auto score = CosineScore(known[index], probe, *probe_norm);
				if (score.has_value() && (match.index < 0 || *score > match.score)) {
					match.index = static_cast<int>(index);
					match.score = *score;
				}
			}
			match.accepted = match.index >= 0 && match.score >= threshold;
			return match;
		}

		[[nodiscard]] inline auto DistanceScore(const std::vector<float> &candidate,
		                                        const std::vector<float> &probe)
		    -> std::optional<float> {
			if (candidate.size() != probe.size() ||
			    !std::ranges::all_of(candidate, [](float value) -> bool {
				    return std::isfinite(value);
			    })) {
				return std::nullopt;
			}
			float sum = 0.0F;
			for (std::size_t element = 0; element < probe.size(); ++element) {
				const float delta = candidate[element] - probe[element];
				sum += delta * delta;
			}
			return std::sqrt(sum);
		}

		[[nodiscard]] inline auto DistanceMatch(const std::vector<std::vector<float>> &known,
		                                        const std::vector<float> &probe, float threshold)
		    -> FaceMatch {
			FaceMatch match{.score = std::numeric_limits<float>::max()};
			if (!std::ranges::all_of(probe, [](float value) -> bool {
				    return std::isfinite(value);
			    })) {
				return match;
			}
			for (std::size_t index = 0; index < known.size(); ++index) {
				const auto score = DistanceScore(known[index], probe);
				if (score.has_value() && *score < match.score) {
					match.index = static_cast<int>(index);
					match.score = *score;
				}
			}
			match.accepted = match.index >= 0 && match.score <= threshold;
			return match;
		}
	}  // namespace face_matching_detail

	[[nodiscard]] inline auto FindBestFaceMatch(const std::vector<std::vector<float>> &known,
	                                            const std::vector<float> &probe, FaceMetric metric,
	                                            float threshold) -> FaceMatch {
		const auto *policy = GetFaceMetricPolicy(metric);
		if (policy == nullptr) {
			return {};
		}
		if (known.empty() || probe.empty()) {
			return {.score =
			            policy->higher_score_is_better ? -1.0F : std::numeric_limits<float>::max()};
		}
		switch (metric) {
			case FaceMetric::kCosine:
				return face_matching_detail::CosineMatch(known, probe, threshold);
			case FaceMetric::kL2:
			case FaceMetric::kL2Norm:
				return face_matching_detail::DistanceMatch(known, probe, threshold);
		}
		return {};
	}

}  // namespace howdy::native
