#include "storage/user_model_codec.hpp"

#include "storage/user_model_limits.hpp"
#include "storage/user_model_status.hpp"
#include "user_model_codec/internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <new>
#include <optional>
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

		auto failure(UserModelStatus status, std::string message) -> UserModelListResult {
			return UserModelListResult{
			    .status        = status,
			    .error_message = std::move(message),
			};
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
			                                                   const std::string &value) -> bool {
				return value.empty() ||
				       yyjson_mut_obj_add_strncpy(document, model, key, value.data(), value.size());
			};
			const auto add_optional_metric = [document,
			                                  model](std::optional<FaceMetric> metric) -> bool {
				if (!metric.has_value()) {
					return true;
				}
				const auto spelling = face_metric_spelling(*metric);
				return !spelling.empty() &&
				       yyjson_mut_obj_add_strncpy(document, model, "metric", spelling.data(),
				                                  spelling.size());
			};
			if (!add_optional_string("backend", entry.backend) ||
			    !add_optional_metric(entry.metric) || !add_optional_string("model", entry.model)) {
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
	                     std::optional<FaceMetric> expected_metric,
	                     const std::string &expected_model, bool strict_shape) -> Document {
		auto decoded = user_model_codec_internal::decode_document_json(
		    input, expected_backend, expected_metric, expected_model, strict_shape);
		if (decoded.doc == nullptr) {
			return Document(std::move(decoded.result));
		}

		std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)> doc_owner(decoded.doc,
		                                                                  yyjson_doc_free);
		try {
			auto impl           = std::make_unique<Document::Impl>();
			impl->immutable_doc = doc_owner.release();
			return {std::move(decoded.result), std::move(impl)};
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
			               std::string(user_model_codec_internal::kStoredEncodingLimitMessage));
		}
		if (!std::ranges::all_of(encoding, [](float value) -> bool {
			    return std::isfinite(value);
		    })) {
			return failure(UserModelStatus::kInvalidShape,
			               std::string(user_model_codec_internal::kStoredEncodingInvalidMessage));
		}
		return UserModelListResult{.status = UserModelStatus::kOk};
	}

}  // namespace howdy::native::user_model_codec
