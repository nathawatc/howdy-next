#include "storage/user_model_codec.hpp"

#include "common/user_names.hpp"
#include "storage/user_model_limits.hpp"

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

namespace howdy::native::user_model_codec {

	struct Document::Impl {
		yyjson_doc     *immutable_doc = nullptr;
		yyjson_mut_doc *mutable_doc   = nullptr;

		~Impl() {
			yyjson_doc_free(immutable_doc);
			yyjson_mut_doc_free(mutable_doc);
		}

		auto ensure_mutable() -> bool {
			if (mutable_doc != nullptr) {
				return true;
			}
			if (immutable_doc != nullptr) {
				mutable_doc = yyjson_doc_mut_copy(immutable_doc, nullptr);
				if (mutable_doc == nullptr) {
					return false;
				}
				yyjson_doc_free(immutable_doc);
				immutable_doc = nullptr;
				return true;
			}

			mutable_doc = yyjson_mut_doc_new(nullptr);
			if (mutable_doc == nullptr) {
				return false;
			}
			auto *root = yyjson_mut_arr(mutable_doc);
			if (root == nullptr) {
				yyjson_mut_doc_free(mutable_doc);
				mutable_doc = nullptr;
				return false;
			}
			yyjson_mut_doc_set_root(mutable_doc, root);
			return true;
		}
	};

	namespace {
		auto json_depth_within_limit(yyjson_val *root) -> bool {
			std::vector<std::pair<yyjson_val *, std::size_t>> pending{{root, 1}};
			while (!pending.empty()) {
				const auto [value, depth] = pending.back();
				pending.pop_back();
				const bool is_container = yyjson_is_arr(value) || yyjson_is_obj(value);
				if (is_container && depth > user_model_limits::kMaxJsonNestingDepth) {
					return false;
				}
				const auto child_depth = [depth](yyjson_val *child) {
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
				                      "Stored face encoding exceeds safety limit"),
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
					                      "Stored face encoding contains an invalid value"),
					};
				}
				const auto number = yyjson_get_num(value);
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

		auto parse_model_entry(yyjson_val *model, const std::string &expected_backend,
		                       const std::string &expected_metric,
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
				                      "Stored face model contains too many encodings"),
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
		                   const std::string &expected_metric, const std::string &expected_model,
		                   bool strict_shape) -> UserModelListResult {
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
				               "Stored face model list exceeds safety limit");
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
						               "Stored face model ID is too large");
					}
					result.next_id = std::numeric_limits<int>::max();
				} else {
					result.next_id = std::max(result.next_id, entry.id + 1);
				}
				result.entries.push_back(std::move(parsed.entry));
			}
			return result;
		}

		auto add_entry_value(yyjson_mut_doc *document, const UserModelEntry &entry)
		    -> yyjson_mut_val * {
			auto *model = yyjson_mut_obj(document);
			if (model == nullptr || !yyjson_mut_obj_add_sint(document, model, "time", entry.time) ||
			    !yyjson_mut_obj_add_strncpy(document, model, "label", entry.label.data(),
			                                entry.label.size()) ||
			    !yyjson_mut_obj_add_sint(document, model, "id", entry.id)) {
				return nullptr;
			}

			auto *data = yyjson_mut_obj_add_arr(document, model, "data");
			if (data == nullptr) {
				return nullptr;
			}
			for (const auto &encoding : entry.encodings) {
				auto *values = yyjson_mut_arr_add_arr(document, data);
				if (values == nullptr) {
					return nullptr;
				}
				for (const float value : encoding) {
					if (!yyjson_mut_arr_add_float(document, values, value)) {
						return nullptr;
					}
				}
			}

			const auto add_optional_string = [document, model](const char        *key,
			                                                   const std::string &value) {
				return value.empty() ||
				       yyjson_mut_obj_add_strncpy(document, model, key, value.data(), value.size());
			};
			if (!add_optional_string("backend", entry.backend) ||
			    !add_optional_string("metric", entry.metric) ||
			    !add_optional_string("model", entry.model)) {
				return nullptr;
			}
			return model;
		}

	}  // namespace

	Document::Document() = default;

	Document::Document(UserModelListResult result_value)
	    : result(std::move(result_value)) {}

	Document::Document(UserModelListResult result_value, std::unique_ptr<Impl> impl)
	    : result(std::move(result_value))
	    , impl_(std::move(impl)) {}

	Document::~Document()                                        = default;
	Document::Document(Document &&) noexcept                     = default;
	auto Document::operator=(Document &&) noexcept -> Document & = default;

	auto decode_document(std::string_view input, const std::string &expected_backend,
	                     const std::string &expected_metric, const std::string &expected_model,
	                     bool strict_shape) -> Document {
		yyjson_doc *parsed = yyjson_read(input.data(), input.size(), YYJSON_READ_ALLOW_BOM);
		if (parsed == nullptr) {
			return Document(
			    failure(UserModelStatus::kParseError, "Failed to parse user model JSON"));
		}

		std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)> parsed_owner(parsed,
		                                                                     yyjson_doc_free);
		try {
			if (!json_depth_within_limit(yyjson_doc_get_root(parsed))) {
				return Document(failure(UserModelStatus::kOversized,
				                        "User model JSON nesting exceeds safety limit"));
			}
			auto result         = parse_entries(yyjson_doc_get_root(parsed), expected_backend,
			                                    expected_metric, expected_model, strict_shape);
			auto impl           = std::make_unique<Document::Impl>();
			impl->immutable_doc = parsed_owner.release();
			return {std::move(result), std::move(impl)};
		} catch (const std::bad_alloc &) {
			return Document{
			    failure(UserModelStatus::kParseError, "Failed to process user model JSON")};
		}
	}

	auto append_entry(Document &document, const UserModelEntry &entry) -> bool {
		try {
			if (document.impl_ == nullptr) {
				document.impl_ = std::make_unique<Document::Impl>();
			}
		} catch (const std::bad_alloc &) {
			return false;
		}
		if (!document.impl_->ensure_mutable()) {
			return false;
		}
		auto *root = yyjson_mut_doc_get_root(document.impl_->mutable_doc);
		if (!yyjson_mut_is_arr(root)) {
			return false;
		}
		auto *model = add_entry_value(document.impl_->mutable_doc, entry);
		return model != nullptr && yyjson_mut_arr_append(root, model);
	}

	auto erase_entry(Document &document, std::size_t index) -> bool {
		if (document.impl_ == nullptr || !document.impl_->ensure_mutable()) {
			return false;
		}
		auto *root = yyjson_mut_doc_get_root(document.impl_->mutable_doc);
		return yyjson_mut_arr_remove(root, index) != nullptr;
	}

	auto is_empty(const Document &document) -> bool {
		if (document.impl_ == nullptr) {
			return true;
		}
		if (document.impl_->mutable_doc != nullptr) {
			return yyjson_mut_arr_size(yyjson_mut_doc_get_root(document.impl_->mutable_doc)) == 0;
		}
		if (document.impl_->immutable_doc != nullptr) {
			return yyjson_arr_size(yyjson_doc_get_root(document.impl_->immutable_doc)) == 0;
		}
		return true;
	}

	auto serialize_document(const Document &document) -> std::optional<std::string> {
		if (document.impl_ == nullptr) {
			return std::nullopt;
		}

		size_t output_size = 0;
		char  *output      = nullptr;
		if (document.impl_->mutable_doc != nullptr) {
			output = yyjson_mut_write_opts(document.impl_->mutable_doc, YYJSON_WRITE_NOFLAG,
			                               nullptr, &output_size, nullptr);
		} else if (document.impl_->immutable_doc != nullptr) {
			output = yyjson_write_opts(document.impl_->immutable_doc, YYJSON_WRITE_NOFLAG, nullptr,
			                           &output_size, nullptr);
		}
		if (output == nullptr) {
			return std::nullopt;
		}

		try {
			std::string serialized(output, output_size);
			std::free(output);
			return serialized;
		} catch (const std::bad_alloc &) {
			std::free(output);
			return std::nullopt;
		}
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
