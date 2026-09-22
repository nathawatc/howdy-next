#pragma once

#include "support/face_metric.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <span>
#include <utility>
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
		[[nodiscard]] inline auto VectorNorm(std::span<const float> values)
		    -> std::optional<float> {
			float squared_norm = 0.0F;
			for (float const value : values) {
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

		[[nodiscard]] inline auto CosineScore(std::span<const float> candidate,
		                                      std::span<const float> probe, float probe_norm)
		    -> std::optional<float> {
			if (candidate.size() != probe.size()) {
				return std::nullopt;
			}
			float squared_norm = 0.0F;
			float dot          = 0.0F;
			auto  probe_it     = probe.cbegin();
			for (const float candidate_value : candidate) {
				if (!std::isfinite(candidate_value)) {
					return std::nullopt;
				}
				dot += candidate_value * *probe_it++;
				squared_norm += candidate_value * candidate_value;
			}
			if (squared_norm == 0.0F || !std::isfinite(squared_norm) || !std::isfinite(dot)) {
				return std::nullopt;
			}
			const float candidate_norm = std::sqrt(std::max(squared_norm, 1.0e-12F));
			const float score          = dot / std::max(candidate_norm * probe_norm, 1.0e-12F);
			return std::isfinite(score) ? std::optional<float>{score} : std::nullopt;
		}

		[[nodiscard]] inline auto PreparedCosineScore(std::span<const float> candidate,
		                                              std::span<const float> probe,
		                                              float candidate_norm, float probe_norm)
		    -> std::optional<float> {
			if (candidate.size() != probe.size()) {
				return std::nullopt;
			}
			float dot      = 0.0F;
			auto  probe_it = probe.begin();
			for (const float value : candidate) {
				dot += value * *probe_it++;
			}
			if (!std::isfinite(dot)) {
				return std::nullopt;
			}
			const float score = dot / std::max(candidate_norm * probe_norm, 1.0e-12F);
			return std::isfinite(score) ? std::optional<float>{score} : std::nullopt;
		}

		[[nodiscard]] inline auto CosineMatch(std::span<const std::vector<float>> known,
		                                      std::span<const float> probe, float threshold,
		                                      std::span<const std::optional<float>> prepared = {})
		    -> FaceMatch {
			const auto probe_norm = VectorNorm(probe);
			if (!probe_norm.has_value()) {
				return {.score = -1.0F};
			}
			FaceMatch   match{.score = -1.0F};
			std::size_t index       = 0;
			auto        prepared_it = prepared.begin();
			for (const auto &candidate : known) {
				std::optional<float> score;
				if (prepared.empty()) {
					score = CosineScore(candidate, probe, *probe_norm);
				} else {
					const auto norm = *prepared_it++;
					if (norm.has_value()) {
						score = PreparedCosineScore(candidate, probe, *norm, *probe_norm);
					}
				}
				if (score.has_value() && (match.index < 0 || *score > match.score)) {
					match.index = static_cast<int>(index);
					match.score = *score;
				}
				++index;
			}
			match.accepted = match.index >= 0 && match.score >= threshold;
			return match;
		}

		[[nodiscard]] inline auto DistanceScore(std::span<const float> candidate,
		                                        std::span<const float> probe,
		                                        bool validated = false) -> std::optional<float> {
			if (candidate.size() != probe.size()) {
				return std::nullopt;
			}
			float sum      = 0.0F;
			auto  probe_it = probe.cbegin();
			for (const float candidate_value : candidate) {
				if (!validated && !std::isfinite(candidate_value)) {
					return std::nullopt;
				}
				const float delta = candidate_value - *probe_it++;
				sum += delta * delta;
			}
			return std::sqrt(sum);
		}

		[[nodiscard]] inline auto DistanceMatch(std::span<const std::vector<float>> known,
		                                        std::span<const float> probe, float threshold,
		                                        std::span<const std::optional<float>> prepared = {})
		    -> FaceMatch {
			FaceMatch match{.score = std::numeric_limits<float>::max()};
			if (!std::ranges::all_of(probe, [](float value) -> bool {
				    return std::isfinite(value);
			    })) {
				return match;
			}
			std::size_t index       = 0;
			auto        prepared_it = prepared.begin();
			for (const auto &candidate : known) {
				std::optional<float> score;
				if (prepared.empty()) {
					score = DistanceScore(candidate, probe);
				} else if ((*prepared_it++).has_value()) {
					score = DistanceScore(candidate, probe, true);
				}
				if (score.has_value() && *score < match.score) {
					match.index = static_cast<int>(index);
					match.score = *score;
				}
				++index;
			}
			match.accepted = match.index >= 0 && match.score <= threshold;
			return match;
		}
	}  // namespace face_matching_detail

	[[nodiscard]] inline auto FindBestFaceMatch(std::span<const std::vector<float>> known,
	                                            std::span<const float> probe, FaceMetric metric,
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

	// Own candidates so cached validation and norms cannot outlive or diverge from them.
	class PreparedFaceMatcher {
	public:
		PreparedFaceMatcher() = default;

		PreparedFaceMatcher(std::vector<std::vector<float>> candidates, FaceMetric metric,
		                    float threshold)
		    : candidates_(std::move(candidates))
		    , metric_(metric)
		    , threshold_(threshold) {
			prepared_.reserve(candidates_.size());
			for (const auto &candidate : candidates_) {
				if (metric_ == FaceMetric::kCosine) {
					prepared_.push_back(face_matching_detail::VectorNorm(candidate));
				} else {
					prepared_.push_back(std::ranges::all_of(candidate,
					                                        [](float value) -> bool {
						                                        return std::isfinite(value);
					                                        })
					                        ? std::optional<float>{1.0F}
					                        : std::nullopt);
				}
			}
		}

		[[nodiscard]] auto Candidates() const -> std::span<const std::vector<float>> {
			return candidates_;
		}

		[[nodiscard]] auto Match(std::span<const float> probe) const -> FaceMatch {
			const auto *policy = GetFaceMetricPolicy(metric_);
			if (policy == nullptr) {
				return {};
			}
			if (candidates_.empty() || probe.empty()) {
				return {.score = policy->higher_score_is_better
				                     ? -1.0F
				                     : std::numeric_limits<float>::max()};
			}
			if (metric_ == FaceMetric::kCosine) {
				return face_matching_detail::CosineMatch(candidates_, probe, threshold_, prepared_);
			}
			return face_matching_detail::DistanceMatch(candidates_, probe, threshold_, prepared_);
		}

	private:
		std::vector<std::vector<float>>   candidates_;
		std::vector<std::optional<float>> prepared_;
		FaceMetric                        metric_    = FaceMetric::kCosine;
		float                             threshold_ = 0.0F;
	};

	using MatchFaceFn = FaceMatch (*)(void *context, const PreparedFaceMatcher &matcher,
	                                  std::span<const float> probe);

	struct FaceMatchingOperations {
		void       *context    = nullptr;
		MatchFaceFn match_face = [](void *, const PreparedFaceMatcher &matcher,
		                            std::span<const float> probe) -> FaceMatch {
			return matcher.Match(probe);
		};
	};

}  // namespace howdy::native
