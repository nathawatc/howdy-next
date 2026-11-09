#pragma once

#include "storage/user_model_status.hpp"
#include "storage/user_model_types.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

// Binary user-model file codec. The byte layout, limits, and label rules are owned by the Rust
// `howdy-user-model` crate; this wrapper is the only C++ code that reaches it.
namespace howdy::native::user_model_codec {

	// Limits enforced by the Rust codec, defined from its generated header.
	extern const std::size_t kMaxFileBytes;
	extern const std::size_t kMaxModels;
	extern const std::size_t kMaxEncodingsPerModel;
	extern const std::size_t kMaxEncodingLength;
	extern const std::size_t kMaxLabelBytes;

	struct EncodeResult {
		UserModelStatus status = UserModelStatus::kOk;
		std::string     error_message;
		std::string     content;
	};

	// Decodes model file bytes. Success is kOk with at least one entry and next_id set past the
	// highest id; every rejected file is a failure status with a message.
	[[nodiscard]] auto Decode(std::string_view content) -> UserModelListResult;

	// Encodes entries (ids strictly ascending) into model file bytes, re-checked by decoding.
	[[nodiscard]] auto Encode(std::span<const UserModelEntry> entries) -> EncodeResult;

	// Whether label is a valid, non-empty model label.
	[[nodiscard]] auto IsValidModelLabel(std::string_view label) -> bool;

	// Length of the longest prefix of text that fits kMaxLabelBytes without splitting a UTF-8
	// sequence.
	[[nodiscard]] auto ModelLabelPrefixLength(std::string_view text) -> std::size_t;

}  // namespace howdy::native::user_model_codec
