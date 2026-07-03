#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace howdy::native {
	inline constexpr std::size_t kSfaceEmbeddingSize = 128;

	enum class FaceEncodingStatus {
		kOk,
		kInferenceError,
		kInvalidOutput,
	};

	struct FaceEncodingResult {
		// Default failure and full payload validation prevent malformed encodings from matching.
		FaceEncodingStatus status = FaceEncodingStatus::kInvalidOutput;
		std::vector<float> encoding;
		std::string        error_message = "Face encoding returned no data";

		[[nodiscard]] auto ok() const -> bool {
			return status == FaceEncodingStatus::kOk && encoding.size() == kSfaceEmbeddingSize &&
			       std::ranges::all_of(encoding, [](float value) {
				       return std::isfinite(value);
			       });
		}
	};

}  // namespace howdy::native
