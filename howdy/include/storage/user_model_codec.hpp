#pragma once

#include "storage/user_models.hpp"

#include <istream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace howdy::native::user_model_codec {

	struct Document {
		UserModelListResult result;
		nlohmann::json      models = nlohmann::json::array();
	};

	auto decode_document(std::istream &input, const std::string &expected_backend,
	                     const std::string &expected_metric, const std::string &expected_model,
	                     bool strict_shape = true) -> Document;
	auto encode_entry(const UserModelEntry &entry) -> nlohmann::json;
	auto serialize_document(const nlohmann::json &models) -> std::string;
	auto validate_encoding(const std::vector<float> &encoding) -> UserModelListResult;

}  // namespace howdy::native::user_model_codec
