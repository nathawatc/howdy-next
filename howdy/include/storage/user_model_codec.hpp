#pragma once

#include "storage/user_models.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

		friend auto decode_document(std::string_view input, const std::string &expected_backend,
		                            const std::string &expected_metric,
		                            const std::string &expected_model, bool strict_shape)
		    -> Document;
		friend auto append_entry(Document &document, const UserModelEntry &entry) -> bool;
		friend auto erase_entry(Document &document, std::size_t index) -> bool;
		friend auto is_empty(const Document &document) -> bool;
		friend auto serialize_document(const Document &document) -> std::optional<std::string>;
	};

	auto decode_document(std::string_view input, const std::string &expected_backend,
	                     const std::string &expected_metric, const std::string &expected_model,
	                     bool strict_shape = true) -> Document;
	auto append_entry(Document &document, const UserModelEntry &entry) -> bool;
	auto erase_entry(Document &document, std::size_t index) -> bool;
	auto is_empty(const Document &document) -> bool;
	auto serialize_document(const Document &document) -> std::optional<std::string>;
	auto validate_encoding(const std::vector<float> &encoding) -> UserModelListResult;

}  // namespace howdy::native::user_model_codec
