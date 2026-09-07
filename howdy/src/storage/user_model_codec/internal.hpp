#pragma once

#include "storage/user_models.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <yyjson.h>

namespace howdy::native::user_model_codec_internal {

	constexpr std::string_view kStoredEncodingLimitMessage =
	    "Stored face encoding exceeds safety limit";
	constexpr std::string_view kStoredEncodingInvalidMessage =
	    "Stored face encoding contains an invalid value";

	struct __attribute__((visibility("hidden"))) DecodedDocumentJson {
		UserModelListResult result;
		yyjson_doc         *doc = nullptr;
	};

	[[nodiscard]] __attribute__((visibility("hidden"))) auto
	DecodeDocumentJson(std::string_view input, const std::string &expected_backend,
	                   std::optional<FaceMetric> expected_metric, const std::string &expected_model,
	                   bool strict_shape) -> DecodedDocumentJson;

}  // namespace howdy::native::user_model_codec_internal
