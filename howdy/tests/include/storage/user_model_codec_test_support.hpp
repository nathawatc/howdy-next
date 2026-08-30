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

	inline auto expect_status(howdy::native::UserModelStatus actual,
	                          howdy::native::UserModelStatus expected, const std::string &message)
	    -> bool {
		return expect(actual == expected, message);
	}

	inline auto model_json(std::string_view id = "7", std::string_view time = "1700000000",
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

	inline auto model_list(const std::vector<std::string> &models) -> std::string {
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

	inline auto decode(std::string_view content, bool strict_shape = true)
	    -> howdy::native::user_model_codec::Document {
		return howdy::native::user_model_codec::decode_document(content, kBackend, kMetric, kModel,
		                                                        strict_shape);
	}

	auto expect_valid_strict_document() -> bool;
	auto expect_new_entry_round_trip() -> bool;
	auto expect_strict_parser_behavior() -> bool;
	auto expect_metric_parsing() -> bool;
	auto expect_duplicate_key_behavior() -> bool;
	auto expect_immutable_to_mutable_conversion() -> bool;
	auto expect_malformed_scalar_fields() -> bool;
	auto expect_strict_shape_contract() -> bool;
	auto expect_compatibility_checks() -> bool;
	auto expect_encoding_validation() -> bool;
	auto expect_additional_shape_edges() -> bool;
	auto expect_document_edge_operations() -> bool;
	auto expect_boundaries_and_limits() -> bool;
	auto expect_multi_model_behavior() -> bool;
	auto expect_unknown_field_preservation() -> bool;
	auto expect_json_depth_limit() -> bool;

}  // namespace howdy::test::user_model_codec
