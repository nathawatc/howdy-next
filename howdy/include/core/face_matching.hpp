#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>
#include <vector>

namespace howdy::native {

	struct FaceMatch {
		int   index    = -1;
		float score    = 0.0F;
		bool  accepted = false;
	};

	[[nodiscard]] inline auto find_best_face_match(const std::vector<std::vector<float>> &known,
	                                               const std::vector<float>              &probe,
	                                               std::string_view metric, float threshold)
	    -> FaceMatch {
		FaceMatch match;
		if (known.empty() || probe.empty()) {
			match.score = metric == "cosine" ? -1.0F : std::numeric_limits<float>::max();
			return match;
		}

		if (metric == "cosine") {
			float best_score = -1.0F;
			int   best_index = -1;
			float probe_norm = 0.0F;
			for (float value : probe) {
				if (!std::isfinite(value)) {
					match.score = -1.0F;
					return match;
				}
				probe_norm += value * value;
			}
			if (probe_norm == 0.0F || !std::isfinite(probe_norm)) {
				match.score = -1.0F;
				return match;
			}
			probe_norm = std::sqrt(std::max(probe_norm, 1.0e-12F));
			if (!std::isfinite(probe_norm)) {
				match.score = -1.0F;
				return match;
			}

			for (std::size_t index = 0; index < known.size(); ++index) {
				const auto &candidate = known[index];
				if (candidate.size() != probe.size()) {
					continue;
				}

				float dot        = 0.0F;
				float known_norm = 0.0F;
				bool  finite     = true;
				for (std::size_t element = 0; element < probe.size(); ++element) {
					if (!std::isfinite(candidate[element])) {
						finite = false;
						break;
					}
					dot += candidate[element] * probe[element];
					known_norm += candidate[element] * candidate[element];
				}
				if (!finite || known_norm == 0.0F || !std::isfinite(known_norm) ||
				    !std::isfinite(dot)) {
					continue;
				}

				known_norm = std::sqrt(std::max(known_norm, 1.0e-12F));
				if (!std::isfinite(known_norm)) {
					continue;
				}
				const float score = dot / std::max(known_norm * probe_norm, 1.0e-12F);
				if (!std::isfinite(score)) {
					continue;
				}
				if (best_index < 0 || score > best_score) {
					best_score = score;
					best_index = static_cast<int>(index);
				}
			}

			match.index    = best_index;
			match.score    = best_score;
			match.accepted = best_index >= 0 && best_score >= threshold;
			return match;
		}

		float best_score = std::numeric_limits<float>::max();
		int   best_index = -1;
		for (std::size_t index = 0; index < known.size(); ++index) {
			const auto &candidate = known[index];
			if (candidate.size() != probe.size()) {
				continue;
			}

			float sum = 0.0F;
			for (std::size_t element = 0; element < probe.size(); ++element) {
				const float delta = candidate[element] - probe[element];
				sum += delta * delta;
			}

			const float score = std::sqrt(sum);
			if (score < best_score) {
				best_score = score;
				best_index = static_cast<int>(index);
			}
		}

		// Equal embeddings have zero distance, which is the best possible distance match.
		match.index    = best_index;
		match.score    = best_score;
		match.accepted = best_index >= 0 && best_score <= threshold;
		return match;
	}

}  // namespace howdy::native
