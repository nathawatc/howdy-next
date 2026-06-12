#include "storage/user_model_codec.hpp"

#include "common/user_names.hpp"
#include "storage/user_model_limits.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace howdy::native::user_model_codec {

	namespace {

		auto failure(UserModelStatus status, std::string message) -> UserModelListResult {
			return UserModelListResult{
			    .status        = status,
			    .error_message = std::move(message),
			};
		}

		auto validate_compatibility(const UserModelEntry &entry,
		                            const std::string    &expected_backend,
		                            const std::string    &expected_metric,
		                            const std::string    &expected_model) -> UserModelListResult {
			if (!expected_backend.empty() && !entry.backend.empty() &&
			    entry.backend != expected_backend) {
				return failure(UserModelStatus::kIncompatibleBackend,
				               "Stored face models use incompatible face-recognition metadata");
			}
			if (!expected_metric.empty() && !entry.metric.empty() &&
			    entry.metric != expected_metric) {
				return failure(UserModelStatus::kIncompatibleMetric,
				               "Stored face models use incompatible face-recognition metadata");
			}
			if (!expected_model.empty() && !entry.model.empty() && entry.model != expected_model) {
				return failure(UserModelStatus::kIncompatibleModel,
				               "Stored face models use incompatible face-recognition metadata");
			}
			return UserModelListResult{.status = UserModelStatus::kOk};
		}

		auto read_int_field(const nlohmann::json &model, const char *key, bool strict_shape)
		    -> std::optional<int> {
			const auto value = model.find(key);
			if (value == model.end()) {
				return strict_shape ? std::nullopt : std::optional<int>(-1);
			}
			if (!value->is_number_integer()) {
				return std::nullopt;
			}
			const auto raw = value->get<long long>();
			if (raw < std::numeric_limits<int>::min() || raw > std::numeric_limits<int>::max()) {
				return std::nullopt;
			}
			return static_cast<int>(raw);
		}

		auto read_time_field(const nlohmann::json &model) -> std::optional<long long> {
			const auto value = model.find("time");
			if (value == model.end()) {
				return 0;
			}
			if (!value->is_number_integer()) {
				return std::nullopt;
			}
			return value->get<long long>();
		}

		auto read_string_field(const nlohmann::json &model, const char *key, bool strict_shape)
		    -> std::optional<std::string> {
			const auto value = model.find(key);
			if (value == model.end()) {
				return std::string();
			}
			if (!value->is_string()) {
				return strict_shape ? std::nullopt : std::optional<std::string>(std::string());
			}
			return value->get<std::string>();
		}

		struct EncodingParseResult {
			UserModelListResult result{.status = UserModelStatus::kOk};
			std::vector<float>  encoding;
		};

		auto parse_encoding(const nlohmann::json &encoding_json) -> EncodingParseResult {
			if (!encoding_json.is_array()) {
				return EncodingParseResult{
				    .result = failure(UserModelStatus::kInvalidShape,
				                      "Stored face encoding is not an array"),
				};
			}
			if (encoding_json.empty() ||
			    encoding_json.size() > user_model_limits::kMaxEncodingLength) {
				return EncodingParseResult{
				    .result = failure(UserModelStatus::kOversized,
				                      "Stored face encoding exceeds safety limit"),
				};
			}

			std::vector<float> encoding;
			encoding.reserve(encoding_json.size());
			for (const auto &value : encoding_json) {
				if (!value.is_number()) {
					return EncodingParseResult{
					    .result = failure(UserModelStatus::kInvalidShape,
					                      "Stored face encoding contains an invalid value"),
					};
				}
				const auto number = value.get<double>();
				if (!std::isfinite(number) || number < -std::numeric_limits<float>::max() ||
				    number > std::numeric_limits<float>::max()) {
					return EncodingParseResult{
					    .result = failure(UserModelStatus::kInvalidShape,
					                      "Stored face encoding contains an invalid value"),
					};
				}
				encoding.push_back(static_cast<float>(number));
			}

			return EncodingParseResult{.encoding = std::move(encoding)};
		}

		struct EntryParseResult {
			UserModelListResult result{.status = UserModelStatus::kOk};
			UserModelEntry      entry;
		};

		auto parse_model_entry(const nlohmann::json &model, const std::string &expected_backend,
		                       const std::string &expected_metric,
		                       const std::string &expected_model, bool strict_shape)
		    -> EntryParseResult {
			if (!model.is_object()) {
				return EntryParseResult{
				    .result = failure(UserModelStatus::kInvalidShape,
				                      "Model file contains an invalid model entry"),
				};
			}

			const auto id = read_int_field(model, "id", strict_shape);
			if (!id.has_value()) {
				return EntryParseResult{
				    .result = failure(UserModelStatus::kInvalidShape,
				                      "Stored face model ID is not an integer"),
				};
			}
			if (strict_shape && *id < 0) {
				return EntryParseResult{
				    .result = failure(UserModelStatus::kInvalidShape,
				                      "Stored face model ID must not be negative"),
				};
			}

			const auto time = read_time_field(model);
			if (!time.has_value()) {
				return EntryParseResult{
				    .result = failure(UserModelStatus::kInvalidShape,
				                      "Stored face model timestamp is not an integer"),
				};
			}

			const auto label = read_string_field(model, "label", strict_shape);
			if (!label.has_value()) {
				return EntryParseResult{
				    .result = failure(UserModelStatus::kInvalidShape,
				                      "Stored face model label is not a string"),
				};
			}
			const auto backend    = read_string_field(model, "backend", strict_shape);
			const auto metric     = read_string_field(model, "metric", strict_shape);
			const auto model_name = read_string_field(model, "model", strict_shape);
			if (!backend.has_value() || !metric.has_value() || !model_name.has_value()) {
				return EntryParseResult{
				    .result = failure(UserModelStatus::kInvalidShape,
				                      "Stored face model metadata is invalid"),
				};
			}

			UserModelEntry entry{
			    .id      = *id,
			    .time    = *time,
			    .label   = *label,
			    .backend = *backend,
			    .metric  = *metric,
			    .model   = *model_name,
			};
			if (!is_valid_model_label(entry.label)) {
				return EntryParseResult{
				    .result = failure(UserModelStatus::kParseError,
				                      "Model label contains unsafe path characters"),
				};
			}
			if (const auto compatibility = validate_compatibility(entry, expected_backend,
			                                                      expected_metric, expected_model);
			    compatibility.status != UserModelStatus::kOk) {
				return EntryParseResult{.result = compatibility};
			}

			const auto data = model.find("data");
			if (data == model.end() || !data->is_array()) {
				if (strict_shape) {
					return EntryParseResult{
					    .result = failure(UserModelStatus::kInvalidShape,
					                      "Stored face model data is not an array"),
					};
				}
				return EntryParseResult{.entry = std::move(entry)};
			}
			if (data->size() > user_model_limits::kMaxEncodingsPerModel) {
				return EntryParseResult{
				    .result = failure(UserModelStatus::kOversized,
				                      "Stored face model contains too many encodings"),
				};
			}

			for (const auto &encoding_json : *data) {
				if (!encoding_json.is_array()) {
					if (strict_shape) {
						return EntryParseResult{
						    .result = failure(UserModelStatus::kInvalidShape,
						                      "Stored face encoding is not an array"),
						};
					}
					continue;
				}
				auto encoding = parse_encoding(encoding_json);
				if (encoding.result.status != UserModelStatus::kOk) {
					return EntryParseResult{.result = std::move(encoding.result)};
				}
				entry.encodings.push_back(std::move(encoding.encoding));
			}

			return EntryParseResult{.entry = std::move(entry)};
		}

		auto parse_entries(const nlohmann::json &models, const std::string &expected_backend,
		                   const std::string &expected_metric, const std::string &expected_model,
		                   bool strict_shape) -> UserModelListResult {
			if (!models.is_array()) {
				return failure(UserModelStatus::kInvalidShape,
				               "Model file is not a valid model list");
			}
			if (models.empty()) {
				return UserModelListResult{.status = UserModelStatus::kNoModel};
			}
			if (models.size() > user_model_limits::kMaxStoredModels) {
				return failure(UserModelStatus::kOversized,
				               "Stored face model list exceeds safety limit");
			}

			UserModelListResult result{.status = UserModelStatus::kOk};
			std::set<int>       seen_ids;
			try {
				for (const auto &model : models) {
					auto parsed = parse_model_entry(model, expected_backend, expected_metric,
					                                expected_model, strict_shape);
					if (parsed.result.status != UserModelStatus::kOk) {
						return parsed.result;
					}
					auto &entry = parsed.entry;
					if (strict_shape && !seen_ids.insert(entry.id).second) {
						return failure(UserModelStatus::kInvalidShape,
						               "Stored face model IDs must be unique");
					}
					if (entry.id == std::numeric_limits<int>::max()) {
						if (strict_shape) {
							return failure(UserModelStatus::kInvalidShape,
							               "Stored face model ID is too large");
						}
						result.next_id = std::numeric_limits<int>::max();
					} else {
						result.next_id = std::max(result.next_id, entry.id + 1);
					}
					result.entries.push_back(std::move(parsed.entry));
				}
			} catch (const nlohmann::json::exception &error) {
				return failure(UserModelStatus::kParseError, error.what());
			}
			return result;
		}

	}  // namespace

	auto decode_document(std::istream &input, const std::string &expected_backend,
	                     const std::string &expected_metric, const std::string &expected_model,
	                     bool strict_shape) -> Document {
		nlohmann::json models;
		try {
			input >> models;
		} catch (const nlohmann::json::exception &error) {
			return Document{
			    .result = failure(UserModelStatus::kParseError, error.what()),
			};
		}
		return Document{
		    .result = parse_entries(models, expected_backend, expected_metric, expected_model,
		                            strict_shape),
		    .models = std::move(models),
		};
	}

	auto encode_entry(const UserModelEntry &entry) -> nlohmann::json {
		nlohmann::json model = {
		    {"time", entry.time},
		    {"label", entry.label},
		    {"id", entry.id},
		    {"data", entry.encodings},
		};
		if (!entry.backend.empty()) {
			model["backend"] = entry.backend;
		}
		if (!entry.metric.empty()) {
			model["metric"] = entry.metric;
		}
		if (!entry.model.empty()) {
			model["model"] = entry.model;
		}
		return model;
	}

	auto serialize_document(const nlohmann::json &models) -> std::string {
		return models.dump();
	}

	auto validate_encoding(const std::vector<float> &encoding) -> UserModelListResult {
		if (encoding.empty() || encoding.size() > user_model_limits::kMaxEncodingLength) {
			return failure(UserModelStatus::kOversized,
			               "Stored face encoding exceeds safety limit");
		}
		if (!std::ranges::all_of(encoding, [](float value) {
			    return std::isfinite(value);
		    })) {
			return failure(UserModelStatus::kInvalidShape,
			               "Stored face encoding contains an invalid value");
		}
		return UserModelListResult{.status = UserModelStatus::kOk};
	}

}  // namespace howdy::native::user_model_codec
