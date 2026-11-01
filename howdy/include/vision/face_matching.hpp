#pragma once

#include "support/face_metric.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
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

		// Empty result marks a vector that cannot be normalized; distance scoring skips it.
		[[nodiscard]] inline auto UnitVector(std::span<const float> values) -> std::vector<float> {
			const auto norm = VectorNorm(values);
			if (!norm.has_value()) {
				return {};
			}
			std::vector<float> unit;
			unit.reserve(values.size());
			for (const float value : values) {
				unit.push_back(value / *norm);
			}
			return unit;
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
		                                      std::span<const std::optional<float>> cosine_norms)
		    -> FaceMatch {
			const auto probe_norm = VectorNorm(probe);
			if (!probe_norm.has_value() || cosine_norms.size() != known.size()) {
				return {.score = -1.0F};
			}
			FaceMatch   match{.score = -1.0F};
			std::size_t index          = 0;
			auto        cosine_norm_it = cosine_norms.begin();
			for (const auto &candidate : known) {
				std::optional<float> score;
				if (const auto norm = *cosine_norm_it++; norm.has_value()) {
					score = PreparedCosineScore(candidate, probe, *norm, *probe_norm);
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

		// Callers pass only candidates already checked for finite values.
		[[nodiscard]] inline auto DistanceScore(std::span<const float> candidate,
		                                        std::span<const float> probe)
		    -> std::optional<float> {
			if (candidate.size() != probe.size()) {
				return std::nullopt;
			}
			float sum      = 0.0F;
			auto  probe_it = probe.cbegin();
			for (const float candidate_value : candidate) {
				const float delta = candidate_value - *probe_it++;
				sum += delta * delta;
			}
			return std::sqrt(sum);
		}

		[[nodiscard]] inline auto DistanceMatch(std::span<const std::vector<float>> known,
		                                        std::span<const float> probe, float threshold,
		                                        std::span<const std::uint8_t> validated)
		    -> FaceMatch {
			FaceMatch match{.score = std::numeric_limits<float>::max()};
			if (validated.size() != known.size() ||
			    !std::ranges::all_of(probe, [](float value) -> bool {
				    return std::isfinite(value);
			    })) {
				return match;
			}
			std::size_t index        = 0;
			auto        validated_it = validated.begin();
			for (const auto &candidate : known) {
				std::optional<float> score;
				if (*validated_it++ != 0U) {
					score = DistanceScore(candidate, probe);
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

	// Own candidates so cached validation and norms cannot outlive or diverge from them.
	class PreparedFaceMatcher {
	public:
		PreparedFaceMatcher() = default;

		PreparedFaceMatcher(std::vector<std::vector<float>> candidates, FaceMetric metric,
		                    float threshold)
		    : candidates_(std::move(candidates))
		    , metric_(metric)
		    , threshold_(threshold) {
			if (metric_ == FaceMetric::kCosine) {
				cosine_norms_.reserve(candidates_.size());
				for (const auto &candidate : candidates_) {
					cosine_norms_.push_back(face_matching_detail::VectorNorm(candidate));
				}
			} else if (metric_ == FaceMetric::kL2Norm) {
				unit_candidates_.reserve(candidates_.size());
				candidate_validity_.reserve(candidates_.size());
				for (const auto &candidate : candidates_) {
					unit_candidates_.push_back(face_matching_detail::UnitVector(candidate));
					candidate_validity_.push_back(
					    static_cast<std::uint8_t>(!unit_candidates_.back().empty()));
				}
			} else {
				candidate_validity_.reserve(candidates_.size());
				for (const auto &candidate : candidates_) {
					candidate_validity_.push_back(static_cast<std::uint8_t>(
					    std::ranges::all_of(candidate, [](float value) -> bool {
						    return std::isfinite(value);
					    })));
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
				return {
				    .score =
				        policy->higher_score_is_better ? -1.0F : std::numeric_limits<float>::max(),
				};
			}
			if (metric_ == FaceMetric::kCosine) {
				return face_matching_detail::CosineMatch(candidates_, probe, threshold_,
				                                         cosine_norms_);
			}
			if (metric_ == FaceMetric::kL2Norm) {
				const auto unit_probe = face_matching_detail::UnitVector(probe);
				if (unit_probe.empty()) {
					return {.score = std::numeric_limits<float>::max()};
				}
				return face_matching_detail::DistanceMatch(unit_candidates_, unit_probe, threshold_,
				                                           candidate_validity_);
			}
			return face_matching_detail::DistanceMatch(candidates_, probe, threshold_,
			                                           candidate_validity_);
		}

	private:
		std::vector<std::vector<float>>   candidates_;
		std::vector<std::optional<float>> cosine_norms_;
		std::vector<std::vector<float>>   unit_candidates_;
		std::vector<std::uint8_t>         candidate_validity_;
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
