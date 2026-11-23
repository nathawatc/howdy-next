#pragma once

#include "storage/user_model_status.hpp"
#include "storage/user_model_types.hpp"

#include <cstddef>
#include <cstdlib>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <yyjson.h>

namespace howdy::native::user_model_codec {

	class Document {
	public:
		Document();
		explicit Document(UserModelListResult result);
		~Document();

		Document(Document &&) noexcept;
		auto operator=(Document &&) noexcept -> Document &;

		Document(const Document &)                     = delete;
		auto operator=(const Document &) -> Document & = delete;

		[[nodiscard]] auto Result() noexcept -> UserModelListResult & {
			return result_;
		}

		[[nodiscard]] auto Result() const noexcept -> const UserModelListResult & {
			return result_;
		}

	private:
		UserModelListResult result_;
		class Impl;
		std::unique_ptr<Impl> impl_;

		Document(UserModelListResult result, std::unique_ptr<Impl> impl);

		friend auto DecodeDocument(std::string_view input, const std::string &expected_backend,
		                           const std::string &expected_model, bool strict_shape)
		    -> Document;
		friend auto AppendEntry(Document &document, const UserModelEntry &entry) -> bool;
		friend auto EraseEntry(Document &document, std::size_t index) -> bool;
		friend auto IsEmpty(const Document &document) -> bool;
		friend auto SerializeDocument(const Document &document) -> std::optional<std::string>;
	};

	auto DecodeDocument(std::string_view input, const std::string &expected_backend,
	                    const std::string &expected_model, bool strict_shape = true) -> Document;
	auto AppendEntry(Document &document, const UserModelEntry &entry) -> bool;
	auto EraseEntry(Document &document, std::size_t index) -> bool;
	auto IsEmpty(const Document &document) -> bool;
	auto SerializeDocument(const Document &document) -> std::optional<std::string>;
	[[nodiscard]] auto ValidateEncoding(std::span<const float> encoding) -> UserModelListResult;

}  // namespace howdy::native::user_model_codec

namespace howdy::native::user_model_codec_internal {

	struct YyjsonDocDeleter {
		void operator()(yyjson_doc *doc) const noexcept {
			yyjson_doc_free(doc);
		}
	};

	struct YyjsonMutDocDeleter {
		void operator()(yyjson_mut_doc *doc) const noexcept {
			yyjson_mut_doc_free(doc);
		}
	};

	struct MallocCharDeleter {
		void operator()(char *ptr) const noexcept {
			std::free(ptr);
		}
	};

	using YyjsonDocPtr    = std::unique_ptr<yyjson_doc, YyjsonDocDeleter>;
	using YyjsonMutDocPtr = std::unique_ptr<yyjson_mut_doc, YyjsonMutDocDeleter>;
	using MallocCharPtr   = std::unique_ptr<char, MallocCharDeleter>;

	constexpr const char *kModelIdField      = "id";
	constexpr const char *kModelTimeField    = "time";
	constexpr const char *kModelLabelField   = "label";
	constexpr const char *kModelDataField    = "data";
	constexpr const char *kModelBackendField = "backend";
	constexpr const char *kModelMetricField  = "metric";
	constexpr const char *kModelNameField    = "model";

	constexpr std::string_view kStoredEncodingLimitMessage =
	    "Stored face encoding exceeds safety limit";
	constexpr std::string_view kStoredEncodingInvalidMessage =
	    "Stored face encoding contains an invalid value";

	[[nodiscard]] inline auto UserModelListFailure(UserModelStatus status, std::string message)
	    -> UserModelListResult {
		return UserModelListResult{
		    .status        = status,
		    .error_message = std::move(message),
		};
	}

	struct __attribute__((visibility("hidden"))) DecodedDocumentJson {
		UserModelListResult result;
		YyjsonDocPtr        doc;
	};

	[[nodiscard]] __attribute__((visibility("hidden"))) auto
	DecodeDocumentJson(std::string_view input, const std::string &expected_backend,
	                   const std::string &expected_model, bool strict_shape) -> DecodedDocumentJson;

#if YYJSON_VERSION_HEX >= 0x000D00
	using ReadOnlyVal = yyjson_val const;
#else
	// yyjson 0.12 read helpers require a mutable pointer even for read-only traversal.
	using ReadOnlyVal = yyjson_val;
#endif

	struct __attribute__((visibility("hidden"))) ExtractedModelFields {
		bool         has_duplicate = false;
		ReadOnlyVal *id            = nullptr;
		ReadOnlyVal *time          = nullptr;
		ReadOnlyVal *label         = nullptr;
		ReadOnlyVal *data          = nullptr;
		ReadOnlyVal *backend       = nullptr;
		ReadOnlyVal *metric        = nullptr;
		ReadOnlyVal *model         = nullptr;
	};

	// Direct members of one stored entry; `check_duplicates` covers unknown keys too.
	[[nodiscard]] __attribute__((visibility("hidden"))) auto
	ExtractModelFields(yyjson_val *model_obj, bool check_duplicates) -> ExtractedModelFields;

}  // namespace howdy::native::user_model_codec_internal
