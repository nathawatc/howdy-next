#pragma once

#include "storage/user_model_types.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct yyjson_doc;

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

		UserModelListResult result;

	private:
		struct Impl;
		std::unique_ptr<Impl> impl_;

		Document(UserModelListResult result, std::unique_ptr<Impl> impl);

		friend auto DecodeDocument(std::string_view input, const std::string &expected_backend,
		                           std::optional<FaceMetric> expected_metric,
		                           const std::string &expected_model, bool strict_shape)
		    -> Document;
		friend auto AppendEntry(Document &document, const UserModelEntry &entry) -> bool;
		friend auto EraseEntry(Document &document, std::size_t index) -> bool;
		friend auto IsEmpty(const Document &document) -> bool;
		friend auto SerializeDocument(const Document &document) -> std::optional<std::string>;
	};

	auto DecodeDocument(std::string_view input, const std::string &expected_backend,
	                    std::optional<FaceMetric> expected_metric,
	                    const std::string &expected_model, bool strict_shape = true) -> Document;
	auto AppendEntry(Document &document, const UserModelEntry &entry) -> bool;
	auto EraseEntry(Document &document, std::size_t index) -> bool;
	auto IsEmpty(const Document &document) -> bool;
	auto SerializeDocument(const Document &document) -> std::optional<std::string>;
	auto ValidateEncoding(const std::vector<float> &encoding) -> UserModelListResult;

}  // namespace howdy::native::user_model_codec

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
