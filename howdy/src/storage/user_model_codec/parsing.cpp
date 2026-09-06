#include "internal.hpp"
#include "storage/user_model_limits.hpp"
#include "support/user_names.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <yyjson.h>

namespace howdy::native::user_model_codec_internal {

	namespace {

		constexpr auto kStoredModelsIncompatibleMessage =
		    "Stored face models use incompatible face-recognition metadata";

		auto json_depth_within_limit(yyjson_val *root) -> bool {
			std::vector<std::pair<yyjson_val *, std::size_t>> pending{{root, 1}};
			while (!pending.empty()) {
				const auto [value, depth] = pending.back();
				pending.pop_back();
				const bool is_container = yyjson_is_arr(value) || yyjson_is_obj(value);
				if (is_container && depth > user_model_limits::kMaxJsonNestingDepth) {
					return false;
				}
				const auto child_depth = [depth](yyjson_val *child) -> std::size_t {
					return depth +
					       static_cast<std::size_t>(yyjson_is_arr(child) || yyjson_is_obj(child));
				};

				if (yyjson_is_arr(value)) {
					size_t      index = 0;
					size_t      count = 0;
					yyjson_val *child = nullptr;
					yyjson_arr_foreach(value, index, count, child) {
						pending.emplace_back(child, child_depth(child));
					}
				} else if (yyjson_is_obj(value)) {
					auto iterator = yyjson_obj_iter_with(value);
					while (auto *key = yyjson_obj_iter_next(&iterator)) {
						auto *child = yyjson_obj_iter_get_val(key);
						pending.emplace_back(child, child_depth(child));
					}
				}
			}
			return true;
		}

		auto failure(UserModelStatus status, std::string message) -> UserModelListResult {
			return UserModelListResult{
			    .status        = status,
			    .error_message = std::move(message),
			};
		}

		auto validate_compatibility(const UserModelEntry            &entry,
		                            const std::string               &expected_backend,
		                            const std::optional<FaceMetric> &expected_metric,
		                            const std::string &expected_model) -> UserModelListResult {
			if (!expected_backend.empty() && !entry.backend.empty() &&
			    entry.backend != expected_backend) {
				return failure(UserModelStatus::kIncompatibleBackend,
				               kStoredModelsIncompatibleMessage);
			}
			if (expected_metric.has_value() && entry.metric.has_value() &&
			    *entry.metric != *expected_metric) {
				return failure(UserModelStatus::kIncompatibleMetric,
				               kStoredModelsIncompatibleMessage);
			}
			if (!expected_model.empty() && !entry.model.empty() && entry.model != expected_model) {
				return failure(UserModelStatus::kIncompatibleModel,
				               kStoredModelsIncompatibleMessage);
			}
			return UserModelListResult{.status = UserModelStatus::kOk};
		}

		auto has_duplicate_direct_key(yyjson_val *model) -> bool {
			std::set<std::string_view> seen_keys;
			auto                       iterator = yyjson_obj_iter_with(model);
			while (auto *key = yyjson_obj_iter_next(&iterator)) {
				const std::string_view name(yyjson_get_str(key), yyjson_get_len(key));
				if (!seen_keys.insert(name).second) {
					return true;
				}
			}
			return false;
		}

		auto find_last_field(yyjson_val *model, std::string_view name) -> yyjson_val * {
			yyjson_val *value    = nullptr;
			auto        iterator = yyjson_obj_iter_with(model);
			while (auto *key = yyjson_obj_iter_next(&iterator)) {
				if (std::string_view(yyjson_get_str(key), yyjson_get_len(key)) == name) {
					value = yyjson_obj_iter_get_val(key);
				}
			}
			return value;
		}

		auto read_int_field(yyjson_val *model, const char *key, bool strict_shape)
		    -> std::optional<int> {
			auto *value = find_last_field(model, key);
			if (value == nullptr) {
				return strict_shape ? std::nullopt : std::optional<int>(-1);
			}
			if (!yyjson_is_int(value)) {
				return std::nullopt;
			}
			if (yyjson_is_uint(value)) {
				const auto raw = yyjson_get_uint(value);
				if (raw > static_cast<unsigned int>(std::numeric_limits<int>::max())) {
					return std::nullopt;
				}
				return static_cast<int>(raw);
			}
			const auto raw = yyjson_get_sint(value);
			if (raw < std::numeric_limits<int>::min() || raw > std::numeric_limits<int>::max()) {
				return std::nullopt;
			}
			return static_cast<int>(raw);
		}

		auto read_time_field(yyjson_val *model) -> std::optional<long long> {
			auto *value = find_last_field(model, "time");
			if (value == nullptr) {
				return 0;
			}
			if (!yyjson_is_int(value)) {
				return std::nullopt;
			}
			if (yyjson_is_uint(value)) {
				const auto raw = yyjson_get_uint(value);
				if (raw > static_cast<unsigned long long>(std::numeric_limits<long long>::max())) {
					return std::nullopt;
				}
				return static_cast<long long>(raw);
			}
			return static_cast<long long>(yyjson_get_sint(value));
		}

		auto read_string_field(yyjson_val *model, const char *key, bool strict_shape)
		    -> std::optional<std::string> {
			auto *value = find_last_field(model, key);
			if (value == nullptr) {
				return std::string();
			}
			if (!yyjson_is_str(value)) {
				return strict_shape ? std::nullopt : std::optional<std::string>(std::string());
			}
			return std::string(yyjson_get_str(value), yyjson_get_len(value));
		}

		struct EncodingParseResult {
			UserModelListResult result{.status = UserModelStatus::kOk};
			std::vector<float>  encoding;
		};

		auto parse_encoding(yyjson_val *encoding_json) -> EncodingParseResult {
			if (!yyjson_is_arr(encoding_json)) {
				return EncodingParseResult{
				    .result = failure(UserModelStatus::kInvalidShape,
				                      "Stored face encoding is not an array"),
				};
			}
			const auto encoding_size = yyjson_arr_size(encoding_json);
			if (encoding_size == 0 || encoding_size > user_model_limits::kMaxEncodingLength) {
				return EncodingParseResult{
				    .result = failure(UserModelStatus::kOversized,
				                      std::string(kStoredEncodingLimitMessage)),
				};
			}

			std::vector<float> encoding;
			encoding.reserve(encoding_size);
			size_t      index = 0;
			size_t      count = 0;
			yyjson_val *value = nullptr;
			yyjson_arr_foreach(encoding_json, index, count, value) {
				if (!yyjson_is_num(value)) {
					return EncodingParseResult{
					    .result = failure(UserModelStatus::kInvalidShape,
					                      std::string(kStoredEncodingInvalidMessage)),
					};
				}
				const auto number = yyjson_get_num(value);
				if (!std::isfinite(number) || number < -std::numeric_limits<float>::max() ||
				    number > std::numeric_limits<float>::max()) {
					return EncodingParseResult{
					    .result = failure(UserModelStatus::kInvalidShape,
					                      std::string(kStoredEncodingInvalidMessage)),
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

		struct MetadataParseResult {
			UserModelListResult       result{.status = UserModelStatus::kOk};
			std::string               backend;
			std::optional<FaceMetric> metric;
			std::string               model;
		};

		auto parse_metadata(yyjson_val *model, bool strict_shape) -> MetadataParseResult {
			const auto backend     = read_string_field(model, "backend", strict_shape);
			const auto metric_text = read_string_field(model, "metric", strict_shape);
			const auto model_name  = read_string_field(model, "model", strict_shape);
			if (!backend.has_value() || !metric_text.has_value() || !model_name.has_value()) {
				return MetadataParseResult{
				    .result = failure(UserModelStatus::kInvalidShape,
				                      "Stored face model metadata is invalid"),
				};
			}
			const std::optional<FaceMetric> metric = metric_text->empty()
			                                             ? std::optional<FaceMetric>{}
			                                             : parse_face_metric(*metric_text);
			if (!metric_text->empty() && !metric.has_value()) {
				return MetadataParseResult{
				    .result = failure(UserModelStatus::kParseError,
				                      "Stored face model contains an unknown face metric"),
				};
			}
			return MetadataParseResult{.backend = *backend, .metric = metric, .model = *model_name};
		}

		auto parse_model_entry(yyjson_val *model, const std::string &expected_backend,
		                       const std::optional<FaceMetric> &expected_metric,
		                       const std::string &expected_model, bool strict_shape)
		    -> EntryParseResult {
			if (!yyjson_is_obj(model)) {
				return EntryParseResult{
				    .result = failure(UserModelStatus::kInvalidShape,
				                      "Model file contains an invalid model entry"),
				};
			}
			if (strict_shape && has_duplicate_direct_key(model)) {
				return EntryParseResult{
				    .result = failure(UserModelStatus::kInvalidShape,
				                      "Stored face model entry contains duplicate keys"),
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
			auto metadata = parse_metadata(model, strict_shape);
			if (metadata.result.status != UserModelStatus::kOk) {
				return EntryParseResult{.result = std::move(metadata.result)};
			}

			UserModelEntry entry{
			    .id      = *id,
			    .time    = *time,
			    .label   = *label,
			    .backend = std::move(metadata.backend),
			    .metric  = metadata.metric,
			    .model   = std::move(metadata.model),
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

			auto *data = find_last_field(model, "data");
			if (!yyjson_is_arr(data)) {
				if (strict_shape) {
					return EntryParseResult{
					    .result = failure(UserModelStatus::kInvalidShape,
					                      "Stored face model data is not an array"),
					};
				}
				return EntryParseResult{.entry = std::move(entry)};
			}
			if (yyjson_arr_size(data) > user_model_limits::kMaxEncodingsPerModel) {
				return EntryParseResult{
				    .result = failure(UserModelStatus::kOversized,
				                      std::string(kStoredEncodingsLimitMessage)),
				};
			}

			size_t      index         = 0;
			size_t      count         = 0;
			yyjson_val *encoding_json = nullptr;
			yyjson_arr_foreach(data, index, count, encoding_json) {
				if (!yyjson_is_arr(encoding_json)) {
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

		auto parse_entries(yyjson_val *models, const std::string &expected_backend,
		                   const std::optional<FaceMetric> &expected_metric,
		                   const std::string &expected_model, bool strict_shape)
		    -> UserModelListResult {
			if (!yyjson_is_arr(models)) {
				return failure(UserModelStatus::kInvalidShape,
				               "Model file is not a valid model list");
			}
			const auto model_count = yyjson_arr_size(models);
			if (model_count == 0) {
				return UserModelListResult{.status = UserModelStatus::kNoModel};
			}
			if (model_count > user_model_limits::kMaxStoredModels) {
				return failure(UserModelStatus::kOversized,
				               std::string(kStoredModelListLimitMessage));
			}

			UserModelListResult result{.status = UserModelStatus::kOk};
			std::set<int>       seen_ids;
			size_t              index = 0;
			size_t              count = 0;
			yyjson_val         *model = nullptr;
			yyjson_arr_foreach(models, index, count, model) {
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
						               std::string(kStoredModelIdLimitMessage));
					}
					result.next_id = std::numeric_limits<int>::max();
				} else {
					result.next_id = std::max(result.next_id, entry.id + 1);
				}
				result.entries.push_back(std::move(parsed.entry));
			}
			return result;
		}

	}  // namespace

	auto decode_document_json(std::string_view input, const std::string &expected_backend,
	                          std::optional<FaceMetric> expected_metric,
	                          const std::string &expected_model, bool strict_shape)
	    -> DecodedDocumentJson {
		yyjson_doc *parsed = yyjson_read(input.data(), input.size(), YYJSON_READ_ALLOW_BOM);
		if (parsed == nullptr) {
			return DecodedDocumentJson{
			    .result = failure(UserModelStatus::kParseError, "Failed to parse user model JSON"),
			    .doc    = nullptr,
			};
		}

		std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)> parsed_owner(parsed,
		                                                                     yyjson_doc_free);
		try {
			if (!json_depth_within_limit(yyjson_doc_get_root(parsed))) {
				return DecodedDocumentJson{
				    .result = failure(UserModelStatus::kOversized,
				                      "User model JSON nesting exceeds safety limit"),
				    .doc    = nullptr,
				};
			}
			auto result = parse_entries(yyjson_doc_get_root(parsed), expected_backend,
			                            expected_metric, expected_model, strict_shape);
			return DecodedDocumentJson{
			    .result = std::move(result),
			    .doc    = parsed_owner.release(),
			};
		} catch (const std::bad_alloc &) {
			return DecodedDocumentJson{
			    .result =
			        failure(UserModelStatus::kParseError, "Failed to process user model JSON"),
			    .doc = nullptr,
			};
		}
	}

}  // namespace howdy::native::user_model_codec_internal
