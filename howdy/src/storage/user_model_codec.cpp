#include "storage/user_model_limits.hpp"
#include "storage/user_model_status.hpp"
#include "storage/user_model_types.hpp"
#include "support/face_metric.hpp"
#include "user_model_codec/internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <yyjson.h>

namespace howdy::native::user_model_codec {

	class Document::Impl {
	public:
		Impl() = default;

		Impl(const Impl &)                     = delete;
		auto operator=(const Impl &) -> Impl & = delete;
		Impl(Impl &&)                          = delete;
		auto operator=(Impl &&) -> Impl &      = delete;
		~Impl()                                = default;

		auto EnsureMutable() -> bool {
			if (std::holds_alternative<user_model_codec_internal::YyjsonMutDocPtr>(doc_)) {
				return Mutable() != nullptr;
			}
			if (const auto *immutable = std::get_if<user_model_codec_internal::YyjsonDocPtr>(&doc_);
			    immutable != nullptr && *immutable != nullptr) {
				user_model_codec_internal::YyjsonMutDocPtr mut(
				    yyjson_doc_mut_copy(immutable->get(), nullptr));
				if (!mut) {
					return false;
				}
				doc_ = std::move(mut);
				return true;
			}

			user_model_codec_internal::YyjsonMutDocPtr mut(yyjson_mut_doc_new(nullptr));
			if (!mut) {
				return false;
			}
			auto *root = yyjson_mut_arr(mut.get());
			if (root == nullptr) {
				return false;
			}
			yyjson_mut_doc_set_root(mut.get(), root);
			doc_ = std::move(mut);
			return true;
		}

		void AdoptImmutable(user_model_codec_internal::YyjsonDocPtr immutable_doc) noexcept {
			doc_ = std::move(immutable_doc);
		}

		[[nodiscard]] auto Immutable() const noexcept -> yyjson_doc * {
			const auto *immutable = std::get_if<user_model_codec_internal::YyjsonDocPtr>(&doc_);
			return immutable != nullptr ? immutable->get() : nullptr;
		}

		[[nodiscard]] auto Mutable() const noexcept -> yyjson_mut_doc * {
			const auto *mut = std::get_if<user_model_codec_internal::YyjsonMutDocPtr>(&doc_);
			return mut != nullptr ? mut->get() : nullptr;
		}

	private:
		std::variant<std::monostate, user_model_codec_internal::YyjsonDocPtr,
		             user_model_codec_internal::YyjsonMutDocPtr>
		    doc_;
	};

	namespace {

		using user_model_codec_internal::UserModelListFailure;

		auto AddEntryValue(yyjson_mut_doc *document, const UserModelEntry &entry)
		    -> yyjson_mut_val * {
			auto *model = yyjson_mut_obj(document);
			if (model == nullptr ||
			    !yyjson_mut_obj_add_sint(document, model,
			                             user_model_codec_internal::kModelTimeField, entry.time) ||
			    !yyjson_mut_obj_add_strncpy(document, model,
			                                user_model_codec_internal::kModelLabelField,
			                                entry.label.data(), entry.label.size()) ||
			    !yyjson_mut_obj_add_sint(document, model, user_model_codec_internal::kModelIdField,
			                             entry.id)) {
				return nullptr;
			}

			auto *data =
			    yyjson_mut_obj_add_arr(document, model, user_model_codec_internal::kModelDataField);
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
				const auto spelling = FaceMetricSpelling(*metric);
				return !spelling.empty() &&
				       yyjson_mut_obj_add_strncpy(document, model,
				                                  user_model_codec_internal::kModelMetricField,
				                                  spelling.data(), spelling.size());
			};
			if (!add_optional_string(user_model_codec_internal::kModelBackendField,
			                         entry.backend) ||
			    !add_optional_metric(entry.metric) ||
			    !add_optional_string(user_model_codec_internal::kModelNameField, entry.model)) {
				return nullptr;
			}
			return model;
		}

	}  // namespace

	Document::Document() = default;

	Document::Document(UserModelListResult result_value)
	    : result_(std::move(result_value)) {}

	Document::Document(UserModelListResult result_value, std::unique_ptr<Impl> impl)
	    : result_(std::move(result_value))
	    , impl_(std::move(impl)) {}

	Document::~Document()                                        = default;
	Document::Document(Document &&) noexcept                     = default;
	auto Document::operator=(Document &&) noexcept -> Document & = default;

	auto DecodeDocument(std::string_view input, const std::string &expected_backend,
	                    const std::string &expected_model, bool strict_shape) -> Document {
		auto decoded = user_model_codec_internal::DecodeDocumentJson(input, expected_backend,
		                                                             expected_model, strict_shape);
		if (!decoded.doc) {
			return Document(std::move(decoded.result));
		}

		try {
			auto impl = std::make_unique<Document::Impl>();
			impl->AdoptImmutable(std::move(decoded.doc));
			return {std::move(decoded.result), std::move(impl)};
		} catch (const std::bad_alloc &) {
			return Document{UserModelListFailure(UserModelStatus::kParseError,
			                                     "Failed to process user model JSON")};
		}
	}

	auto AppendEntry(Document &document, const UserModelEntry &entry) -> bool {
		// Allocated before the JSON array changes, so the two cannot disagree.
		std::optional<UserModelEntry> stored_entry;
		try {
			if (document.impl_ == nullptr) {
				document.impl_ = std::make_unique<Document::Impl>();
			}
			stored_entry.emplace(entry);
			document.result_.entries.reserve(document.result_.entries.size() + 1);
		} catch (const std::bad_alloc &) {
			return false;
		}
		if (!document.impl_->EnsureMutable()) {
			return false;
		}
		auto *root = yyjson_mut_doc_get_root(document.impl_->Mutable());
		if (!yyjson_mut_is_arr(root)) {
			return false;
		}
		auto *model = AddEntryValue(document.impl_->Mutable(), entry);
		if (model == nullptr || !yyjson_mut_arr_append(root, model)) {
			return false;
		}
		document.result_.status  = UserModelStatus::kOk;
		document.result_.next_id = entry.id == std::numeric_limits<int>::max()
		                               ? std::numeric_limits<int>::max()
		                               : std::max(document.result_.next_id, entry.id + 1);
		document.result_.entries.push_back(std::move(*stored_entry));
		return true;
	}

	auto EraseEntry(Document &document, std::size_t index) -> bool {
		if (document.impl_ == nullptr || !document.impl_->EnsureMutable()) {
			return false;
		}
		auto *root = yyjson_mut_doc_get_root(document.impl_->Mutable());
		if (yyjson_mut_arr_remove(root, index) == nullptr) {
			return false;
		}
		if (index < document.result_.entries.size()) {
			document.result_.entries.erase(document.result_.entries.begin() +
			                               static_cast<std::ptrdiff_t>(index));
		}
		return true;
	}

	auto IsEmpty(const Document &document) -> bool {
		if (document.impl_ == nullptr) {
			return true;
		}
		if (document.impl_->Mutable() != nullptr) {
			return yyjson_mut_arr_size(yyjson_mut_doc_get_root(document.impl_->Mutable())) == 0;
		}
		if (document.impl_->Immutable() != nullptr) {
			return yyjson_arr_size(yyjson_doc_get_root(document.impl_->Immutable())) == 0;
		}
		return true;
	}

	auto SerializeDocument(const Document &document) -> std::optional<std::string> {
		if (document.impl_ == nullptr) {
			return std::nullopt;
		}

		size_t output_size = 0;
		char  *output      = nullptr;
		if (document.impl_->Mutable() != nullptr) {
			output = yyjson_mut_write_opts(document.impl_->Mutable(), YYJSON_WRITE_NOFLAG, nullptr,
			                               &output_size, nullptr);
		} else if (document.impl_->Immutable() != nullptr) {
			output = yyjson_write_opts(document.impl_->Immutable(), YYJSON_WRITE_NOFLAG, nullptr,
			                           &output_size, nullptr);
		}
		const user_model_codec_internal::MallocCharPtr output_owner(output);
		if (!output_owner) {
			return std::nullopt;
		}

		try {
			return std::string(output_owner.get(), output_size);
		} catch (const std::bad_alloc &) {
			return std::nullopt;
		}
	}

	auto ValidateEncoding(std::span<const float> encoding) -> UserModelListResult {
		if (encoding.empty() || encoding.size() > user_model_limits::kMaxEncodingLength) {
			return UserModelListFailure(
			    UserModelStatus::kOversized,
			    std::string(user_model_codec_internal::kStoredEncodingLimitMessage));
		}
		if (!std::ranges::all_of(encoding, [](float value) -> bool {
			    return std::isfinite(value);
		    })) {
			return UserModelListFailure(
			    UserModelStatus::kInvalidShape,
			    std::string(user_model_codec_internal::kStoredEncodingInvalidMessage));
		}
		return UserModelListResult{.status = UserModelStatus::kOk};
	}

}  // namespace howdy::native::user_model_codec
