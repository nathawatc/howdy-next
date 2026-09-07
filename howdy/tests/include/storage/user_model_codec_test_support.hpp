#pragma once

#include "storage/user_model_codec.hpp"
#include "test_support.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace howdy::test::user_model_codec {

	using howdy::test::expect;

	constexpr auto kBackend = "opencv_dnn_sface";
	constexpr auto kMetric  = howdy::native::FaceMetric::kCosine;
	constexpr auto kModel   = "sface.onnx";

	inline auto ExpectStatus(howdy::native::UserModelStatus actual,
	                         howdy::native::UserModelStatus expected, const std::string &message)
	    -> bool {
		return expect(actual == expected, message);
	}

	inline auto ModelJson(std::string_view id = "7", std::string_view time = "1700000000",
	                      std::string_view label   = R"("Office camera")",
	                      std::string_view backend = R"("opencv_dnn_sface")",
	                      std::string_view metric  = R"("cosine")",
	                      std::string_view model   = R"("sface.onnx")",
	                      std::string_view data = "[[1.0,2.0,-3.5]]", std::string_view extra = {})
	    -> std::string {
		return "{\"id\":" + std::string(id) + ",\"time\":" + std::string(time) +
		       ",\"label\":" + std::string(label) + ",\"backend\":" + std::string(backend) +
		       ",\"metric\":" + std::string(metric) + ",\"model\":" + std::string(model) +
		       ",\"data\":" + std::string(data) + std::string(extra) + "}";
	}

	inline auto ModelList(const std::vector<std::string> &models) -> std::string {
		std::string output = "[";
		for (std::size_t index = 0; index < models.size(); ++index) {
			if (index > 0) {
				output += ',';
			}
			output += models[index];
		}
		output += ']';
		return output;
	}

	inline auto Decode(std::string_view content, bool strict_shape = true)
	    -> howdy::native::user_model_codec::Document {
		return howdy::native::user_model_codec::DecodeDocument(content, kBackend, kMetric, kModel,
		                                                       strict_shape);
	}

	auto ExpectValidStrictDocument() -> bool;
	auto ExpectNewEntryRoundTrip() -> bool;
	auto ExpectStrictParserBehavior() -> bool;
	auto ExpectMetricParsing() -> bool;
	auto ExpectDuplicateKeyBehavior() -> bool;
	auto ExpectImmutableToMutableConversion() -> bool;
	auto ExpectMalformedScalarFields() -> bool;
	auto ExpectStrictShapeContract() -> bool;
	auto ExpectCompatibilityChecks() -> bool;
	auto ExpectEncodingValidation() -> bool;
	auto ExpectAdditionalShapeEdges() -> bool;
	auto ExpectDocumentEdgeOperations() -> bool;
	auto ExpectBoundariesAndLimits() -> bool;
	auto ExpectMultiModelBehavior() -> bool;
	auto ExpectUnknownFieldPreservation() -> bool;
	auto ExpectJsonDepthLimit() -> bool;

}  // namespace howdy::test::user_model_codec
