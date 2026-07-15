#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>

namespace howdy::native {

	struct FaceMatch {
		int   index    = -1;
		float score    = 0.0F;
		bool  accepted = false;
	};

	namespace face_matching_detail {
		[[nodiscard]] inline auto vector_norm(const std::vector<float> &values)
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

		[[nodiscard]] inline auto cosine_score(const std::vector<float> &candidate,
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
			const auto candidate_norm = vector_norm(candidate);
			if (!candidate_norm.has_value() || !std::isfinite(dot)) {
				return std::nullopt;
			}
			const float score = dot / std::max(*candidate_norm * probe_norm, 1.0e-12F);
			return std::isfinite(score) ? std::optional<float>{score} : std::nullopt;
		}

		[[nodiscard]] inline auto cosine_match(const std::vector<std::vector<float>> &known,
		                                       const std::vector<float> &probe, float threshold)
		    -> FaceMatch {
			const auto probe_norm = vector_norm(probe);
			if (!probe_norm.has_value()) {
				return {.score = -1.0F};
			}
			FaceMatch match{.score = -1.0F};
			for (std::size_t index = 0; index < known.size(); ++index) {
				const auto score = cosine_score(known[index], probe, *probe_norm);
				if (score.has_value() && (match.index < 0 || *score > match.score)) {
					match.index = static_cast<int>(index);
					match.score = *score;
				}
			}
			match.accepted = match.index >= 0 && match.score >= threshold;
			return match;
		}

		[[nodiscard]] inline auto distance_score(const std::vector<float> &candidate,
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

		[[nodiscard]] inline auto distance_match(const std::vector<std::vector<float>> &known,
		                                         const std::vector<float> &probe, float threshold)
		    -> FaceMatch {
			FaceMatch match{.score = std::numeric_limits<float>::max()};
			if (!std::ranges::all_of(probe, [](float value) -> bool {
				    return std::isfinite(value);
			    })) {
				return match;
			}
			for (std::size_t index = 0; index < known.size(); ++index) {
				const auto score = distance_score(known[index], probe);
				if (score.has_value() && *score < match.score) {
					match.index = static_cast<int>(index);
					match.score = *score;
				}
			}
			match.accepted = match.index >= 0 && match.score <= threshold;
			return match;
		}
	}  // namespace face_matching_detail

	[[nodiscard]] inline auto find_best_face_match(const std::vector<std::vector<float>> &known,
	                                               const std::vector<float>              &probe,
	                                               std::string_view metric, float threshold)
	    -> FaceMatch {
		if (known.empty() || probe.empty()) {
			return {.score = metric == "cosine" ? -1.0F : std::numeric_limits<float>::max()};
		}
		if (metric == "cosine") {
			return face_matching_detail::cosine_match(known, probe, threshold);
		}
		return face_matching_detail::distance_match(known, probe, threshold);
	}

}  // namespace howdy::native
