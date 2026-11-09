#include "storage/user_model_codec.hpp"

#include "storage/user_model_status.hpp"
#include "storage/user_model_types.hpp"
#include "support/face_metric.hpp"

#include "howdy_ffi.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace howdy::native::user_model_codec {

	const std::size_t kMaxFileBytes         = HOWDY_USER_MODEL_MAX_FILE_BYTES;
	const std::size_t kMaxModels            = HOWDY_USER_MODEL_MAX_MODELS;
	const std::size_t kMaxEncodingsPerModel = HOWDY_USER_MODEL_MAX_ENCODINGS_PER_MODEL;
	const std::size_t kMaxEncodingLength    = HOWDY_USER_MODEL_MAX_ENCODING_LEN;
	const std::size_t kMaxLabelBytes        = HOWDY_USER_MODEL_MAX_LABEL_BYTES;

	namespace {

		struct DecodedDeleter {
			void operator()(HowdyUserModelDecoded *decoded) const noexcept {
				howdy_user_model_decoded_free(decoded);
			}
		};

		struct EncodedDeleter {
			void operator()(HowdyUserModelEncoded *encoded) const noexcept {
				howdy_user_model_encoded_free(encoded);
			}
		};

		using DecodedHandle = std::unique_ptr<HowdyUserModelDecoded, DecodedDeleter>;
		using EncodedHandle = std::unique_ptr<HowdyUserModelEncoded, EncodedDeleter>;

		constexpr std::string_view kDecodeFailedMessage = "Failed to process user model file";
		constexpr std::string_view kEncodeFailedMessage = "Failed to encode user model file";

		struct Failure {
			UserModelStatus  status;
			std::string_view message;
		};

		// The one mapping from codec errors to C++ statuses and user-facing messages.
		auto DescribeError(HowdyUserModelErrorKind kind) -> Failure {
			switch (kind) {
				case HOWDY_USER_MODEL_ERROR_KIND_TOO_LARGE:
					return {
					    .status  = UserModelStatus::kOversized,
					    .message = "User model file exceeds safety limit",
					};
				case HOWDY_USER_MODEL_ERROR_KIND_TRUNCATED:
					return {
					    .status  = UserModelStatus::kParseError,
					    .message = "User model file is truncated",
					};
				case HOWDY_USER_MODEL_ERROR_KIND_BAD_MAGIC:
					return {
					    .status  = UserModelStatus::kParseError,
					    .message = "File is not a Howdy user model file",
					};
				case HOWDY_USER_MODEL_ERROR_KIND_UNSUPPORTED_VERSION:
					return {
					    .status  = UserModelStatus::kParseError,
					    .message = "User model file format version is not supported",
					};
				case HOWDY_USER_MODEL_ERROR_KIND_CHECKSUM_MISMATCH:
					return {
					    .status  = UserModelStatus::kParseError,
					    .message = "User model file checksum does not match; the file is corrupted",
					};
				case HOWDY_USER_MODEL_ERROR_KIND_TRAILING_BYTES:
					return {
					    .status  = UserModelStatus::kParseError,
					    .message = "User model file has unexpected trailing data",
					};
				case HOWDY_USER_MODEL_ERROR_KIND_NO_MODELS:
					return {
					    .status  = UserModelStatus::kInvalidShape,
					    .message = "User model file contains no models",
					};
				case HOWDY_USER_MODEL_ERROR_KIND_TOO_MANY_MODELS:
					return {
					    .status  = UserModelStatus::kOversized,
					    .message = kStoredModelListLimitMessage,
					};
				case HOWDY_USER_MODEL_ERROR_KIND_ID_OUT_OF_RANGE:
					return {
					    .status  = UserModelStatus::kInvalidShape,
					    .message = kStoredModelIdLimitMessage,
					};
				case HOWDY_USER_MODEL_ERROR_KIND_ID_NOT_ASCENDING:
					return {
					    .status  = UserModelStatus::kInvalidShape,
					    .message = "Stored face model IDs must be unique and ascending",
					};
				case HOWDY_USER_MODEL_ERROR_KIND_INVALID_METRIC:
					return {
					    .status  = UserModelStatus::kParseError,
					    .message = "Stored face model contains an unknown face metric",
					};
				case HOWDY_USER_MODEL_ERROR_KIND_INVALID_LABEL:
					return {
					    .status  = UserModelStatus::kInvalidShape,
					    .message = "Stored face model label is invalid",
					};
				case HOWDY_USER_MODEL_ERROR_KIND_INVALID_METADATA:
					return {
					    .status  = UserModelStatus::kInvalidShape,
					    .message = "Stored face model metadata is invalid",
					};
				case HOWDY_USER_MODEL_ERROR_KIND_NO_ENCODINGS:
					return {
					    .status  = UserModelStatus::kInvalidShape,
					    .message = "Stored face model contains no encodings",
					};
				case HOWDY_USER_MODEL_ERROR_KIND_TOO_MANY_ENCODINGS:
					return {
					    .status  = UserModelStatus::kOversized,
					    .message = kStoredEncodingsLimitMessage,
					};
				case HOWDY_USER_MODEL_ERROR_KIND_ENCODING_LENGTH:
					return {
					    .status  = UserModelStatus::kOversized,
					    .message = "Stored face encoding length is outside the safety limit",
					};
				case HOWDY_USER_MODEL_ERROR_KIND_ENCODING_LENGTH_MISMATCH:
					return {
					    .status  = UserModelStatus::kInvalidShape,
					    .message = "Stored face encodings differ in length",
					};
				case HOWDY_USER_MODEL_ERROR_KIND_NON_FINITE_VALUE:
					return {
					    .status  = UserModelStatus::kInvalidShape,
					    .message = "Stored face encoding contains an invalid value",
					};
				case HOWDY_USER_MODEL_ERROR_KIND_ENCODE_MISMATCH:
					return {
					    .status  = UserModelStatus::kWriteFailed,
					    .message = kEncodeFailedMessage,
					};
			}
			return {
			    .status  = UserModelStatus::kParseError,
			    .message = kDecodeFailedMessage,
			};
		}

		auto View(std::string_view bytes) -> HowdyStr {
			return {.ptr = bytes.data(), .len = bytes.size()};
		}

		auto Text(HowdyStr text) -> std::string {
			return text.len == 0 ? std::string{} : std::string(text.ptr, text.len);
		}

		auto MetricCode(FaceMetric metric) -> std::uint32_t {
			switch (metric) {
				case FaceMetric::kCosine:
					return HOWDY_USER_MODEL_METRIC_COSINE;
				case FaceMetric::kL2:
					return HOWDY_USER_MODEL_METRIC_L2;
				case FaceMetric::kL2Norm:
					return HOWDY_USER_MODEL_METRIC_L2_NORM;
			}
			return 0;  // Not a metric code; Rust rejects it as an invalid argument.
		}

		auto ToFaceMetric(HowdyUserModelMetric metric) -> std::optional<FaceMetric> {
			switch (metric) {
				case HOWDY_USER_MODEL_METRIC_COSINE:
					return FaceMetric::kCosine;
				case HOWDY_USER_MODEL_METRIC_L2:
					return FaceMetric::kL2;
				case HOWDY_USER_MODEL_METRIC_L2_NORM:
					return FaceMetric::kL2Norm;
			}
			return std::nullopt;
		}

		auto DecodeFailure(UserModelStatus status, std::string_view message)
		    -> UserModelListResult {
			return UserModelListResult{
			    .status        = status,
			    .error_message = std::string(message),
			};
		}

		auto EncodeFailure(UserModelStatus status, std::string_view message) -> EncodeResult {
			return EncodeResult{
			    .status        = status,
			    .error_message = std::string(message),
			};
		}

		auto ReadEntry(const HowdyUserModelDecoded *decoded, std::size_t index)
		    -> std::optional<UserModelEntry> {
			HowdyUserModelEntryView view{.metric = HOWDY_USER_MODEL_METRIC_COSINE};
			if (howdy_user_model_decoded_entry(decoded, index, &view) != HOWDY_FFI_STATUS_OK) {
				return std::nullopt;
			}
			const auto metric = ToFaceMetric(view.metric);
			if (!metric.has_value() || view.values == nullptr) {
				return std::nullopt;
			}
			UserModelEntry entry{
			    .id      = static_cast<int>(view.id),
			    .time    = view.time,
			    .label   = Text(view.label),
			    .backend = Text(view.backend),
			    .metric  = *metric,
			    .model   = Text(view.model),
			};
			const std::span<const float> values(view.values,
			                                    view.encoding_count * view.encoding_len);
			entry.encodings.reserve(view.encoding_count);
			for (std::size_t offset = 0; offset < values.size(); offset += view.encoding_len) {
				const auto encoding = values.subspan(offset, view.encoding_len);
				entry.encodings.emplace_back(encoding.begin(), encoding.end());
			}
			return entry;
		}

	}  // namespace

	auto Decode(std::string_view content) -> UserModelListResult {
		HowdyUserModelDecoded *raw    = nullptr;
		const auto             status = howdy_user_model_decode(View(content), &raw);
		const DecodedHandle    handle(raw);
		if (status == HOWDY_FFI_STATUS_TOO_LARGE) {
			return DecodeFailure(UserModelStatus::kOversized,
			                     DescribeError(HOWDY_USER_MODEL_ERROR_KIND_TOO_LARGE).message);
		}
		if (status != HOWDY_FFI_STATUS_OK || handle == nullptr) {
			return DecodeFailure(UserModelStatus::kParseError, kDecodeFailedMessage);
		}
		bool                    has_error = true;
		HowdyUserModelErrorKind kind      = HOWDY_USER_MODEL_ERROR_KIND_TRUNCATED;
		if (howdy_user_model_decoded_error(handle.get(), &has_error, &kind) !=
		    HOWDY_FFI_STATUS_OK) {
			return DecodeFailure(UserModelStatus::kParseError, kDecodeFailedMessage);
		}
		if (has_error) {
			const auto failure = DescribeError(kind);
			return DecodeFailure(failure.status, failure.message);
		}
		std::size_t count = 0;
		if (howdy_user_model_decoded_count(handle.get(), &count) != HOWDY_FFI_STATUS_OK ||
		    count == 0) {
			return DecodeFailure(UserModelStatus::kParseError, kDecodeFailedMessage);
		}

		UserModelListResult result{.status = UserModelStatus::kOk};
		result.entries.reserve(count);
		for (std::size_t index = 0; index < count; ++index) {
			auto entry = ReadEntry(handle.get(), index);
			if (!entry.has_value()) {
				return DecodeFailure(UserModelStatus::kParseError, kDecodeFailedMessage);
			}
			result.entries.push_back(std::move(*entry));
		}
		// Ids are strictly ascending and at most HOWDY_USER_MODEL_MAX_ID, so this fits an int.
		result.next_id = result.entries.back().id + 1;
		return result;
	}

	auto Encode(std::span<const UserModelEntry> entries) -> EncodeResult {
		std::vector<std::vector<HowdyUserModelEncoding>> encodings;
		std::vector<HowdyUserModelEntryInput>            inputs;
		encodings.reserve(entries.size());
		inputs.reserve(entries.size());
		for (const auto &entry : entries) {
			if (entry.id < 0) {
				return EncodeFailure(UserModelStatus::kInvalidShape,
				                     "Stored face model ID must not be negative");
			}
			auto &views = encodings.emplace_back();
			views.reserve(entry.encodings.size());
			for (const auto &encoding : entry.encodings) {
				views.push_back({.values = encoding.data(), .len = encoding.size()});
			}
			inputs.push_back({
			    .id             = static_cast<std::uint32_t>(entry.id),
			    .time           = entry.time,
			    .metric         = MetricCode(entry.metric),
			    .label          = View(entry.label),
			    .backend        = View(entry.backend),
			    .model          = View(entry.model),
			    .encodings      = views.data(),
			    .encoding_count = views.size(),
			});
		}

		HowdyUserModelEncoded *raw    = nullptr;
		const auto             status = howdy_user_model_encode(inputs.data(), inputs.size(), &raw);
		const EncodedHandle    handle(raw);
		if (status != HOWDY_FFI_STATUS_OK || handle == nullptr) {
			return EncodeFailure(UserModelStatus::kWriteFailed, kEncodeFailedMessage);
		}
		bool                    has_error = true;
		HowdyUserModelErrorKind kind      = HOWDY_USER_MODEL_ERROR_KIND_ENCODE_MISMATCH;
		if (howdy_user_model_encoded_error(handle.get(), &has_error, &kind) !=
		    HOWDY_FFI_STATUS_OK) {
			return EncodeFailure(UserModelStatus::kWriteFailed, kEncodeFailedMessage);
		}
		if (has_error) {
			const auto failure = DescribeError(kind);
			return EncodeFailure(failure.status, failure.message);
		}
		HowdyStr content{};
		if (howdy_user_model_encoded_content(handle.get(), &content) != HOWDY_FFI_STATUS_OK) {
			return EncodeFailure(UserModelStatus::kWriteFailed, kEncodeFailedMessage);
		}
		return EncodeResult{.content = Text(content)};
	}

	auto IsValidModelLabel(std::string_view label) -> bool {
		bool valid = false;
		return howdy_user_model_is_valid_label(View(label), &valid) == HOWDY_FFI_STATUS_OK && valid;
	}

	auto ModelLabelPrefixLength(std::string_view text) -> std::size_t {
		std::size_t length = 0;
		if (howdy_user_model_label_prefix_len(View(text), &length) != HOWDY_FFI_STATUS_OK) {
			return 0;
		}
		return length;
	}

}  // namespace howdy::native::user_model_codec
