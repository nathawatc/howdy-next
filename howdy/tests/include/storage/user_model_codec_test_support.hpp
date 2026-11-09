#pragma once

#include "storage/user_model_codec.hpp"
#include "storage/user_model_status.hpp"
#include "storage/user_model_types.hpp"
#include "support/face_metric.hpp"

#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <utility>
#include <vector>

// Builds binary user-model fixtures through the production codec, so tests never carry a second
// encoder.
namespace howdy::test::user_model_codec {

	inline constexpr auto         kBackend = "opencv_dnn_sface";
	inline constexpr auto         kModel   = "sface.onnx";
	inline constexpr std::int64_t kTime    = 1;

	inline auto ModelEntry(int id, std::string label = "Model",
	                       std::vector<std::vector<float>> encodings = {{0.1F, 0.2F}})
	    -> howdy::native::UserModelEntry {
		return howdy::native::UserModelEntry{
		    .id        = id,
		    .time      = kTime,
		    .label     = std::move(label),
		    .backend   = kBackend,
		    .metric    = howdy::native::FaceMetric::kCosine,
		    .model     = kModel,
		    .encodings = std::move(encodings),
		};
	}

	// Encoded model file bytes for entries. A rejected fixture reports a failure and yields no
	// bytes, which the calling test then fails on.
	inline auto EncodeModels(std::span<const howdy::native::UserModelEntry> entries)
	    -> std::string {
		auto result = howdy::native::user_model_codec::Encode(entries);
		if (result.status != howdy::native::UserModelStatus::kOk) {
			std::cerr << "FAIL: test fixture failed to encode: " << result.error_message << '\n';
			return {};
		}
		return std::move(result.content);
	}

	inline auto EncodeModels(const std::vector<howdy::native::UserModelEntry> &entries)
	    -> std::string {
		return EncodeModels(std::span<const howdy::native::UserModelEntry>(entries));
	}

	// content with its middle byte changed, so a valid model file fails its checksum.
	inline auto CorruptMiddleByte(std::string content) -> std::string {
		if (!content.empty()) {
			auto &byte = content.at(content.size() / 2);
			byte       = byte == '\0' ? '\1' : '\0';
		}
		return content;
	}

	// A model file with valid framing that fails its checksum.
	inline auto CorruptModelFile() -> std::string {
		return CorruptMiddleByte(EncodeModels(std::vector{ModelEntry(0)}));
	}

}  // namespace howdy::test::user_model_codec
