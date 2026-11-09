#include "storage/user_model_codec.hpp"
#include "storage/user_model_codec_test_support.hpp"
#include "storage/user_model_status.hpp"
#include "storage/user_model_types.hpp"
#include "support/face_metric.hpp"
#include "test_support.hpp"

#include <array>
#include <cstddef>
#include <exception>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

	using howdy::native::FaceMetric;
	using howdy::native::UserModelEntry;
	using howdy::native::UserModelStatus;
	using howdy::test::Expect;
	using howdy::test::user_model_codec::EncodeModels;
	using howdy::test::user_model_codec::ModelEntry;
	namespace codec = howdy::native::user_model_codec;

	auto ExpectDecodeFailure(std::string_view content, UserModelStatus status,
	                         std::string_view message, std::string_view what) -> bool {
		const auto result = codec::Decode(content);
		bool       ok     = Expect(result.status == status, std::string(what) + ": status");
		ok &= Expect(result.error_message == message, std::string(what) + ": message");
		ok &= Expect(result.entries.empty(), std::string(what) + ": no entries");
		return ok;
	}

	auto ExpectEncodeFailure(const std::vector<UserModelEntry> &entries, UserModelStatus status,
	                         std::string_view message, std::string_view what) -> bool {
		const auto result = codec::Encode(entries);
		bool       ok     = Expect(result.status == status, std::string(what) + ": status");
		ok &= Expect(result.error_message == message, std::string(what) + ": message");
		ok &= Expect(result.content.empty(), std::string(what) + ": no content");
		return ok;
	}

	auto ExpectRoundTrip() -> bool {
		const auto first  = ModelEntry(0, "Office camera", {{0.5F, -0.0F}, {1.5F, 2.5F}});
		auto       second = ModelEntry(4, "Office camera", {{-1.0F, 3.0F, 4.0F}});
		second.metric     = FaceMetric::kL2Norm;
		second.time       = -5;
		const std::vector<UserModelEntry> entries{first, second};

		const auto content = EncodeModels(entries);
		const auto decoded = codec::Decode(content);
		bool       ok      = Expect(decoded.status == UserModelStatus::kOk, "round trip decodes");
		ok &= Expect(decoded.entries.size() == 2, "round trip keeps entry count");
		ok &= Expect(decoded.next_id == 5, "next_id follows the highest id");
		if (decoded.entries.size() != 2) {
			return false;
		}
		const auto &one = decoded.entries.front();
		const auto &two = decoded.entries.back();
		ok &= Expect(one.id == 0 && one.time == howdy::test::user_model_codec::kTime,
		             "id and time round-trip");
		ok &= Expect(one.label == "Office camera" && two.label == one.label,
		             "repeated labels round-trip");
		ok &= Expect(one.backend == "opencv_dnn_sface" && one.model == "sface.onnx",
		             "metadata round-trips");
		ok &= Expect(one.metric == FaceMetric::kCosine && two.metric == FaceMetric::kL2Norm,
		             "metrics round-trip");
		ok &= Expect(one.encodings == first.encodings && two.encodings == second.encodings,
		             "encodings round-trip");
		ok &= Expect(two.time == -5, "negative time round-trips");
		ok &= Expect(EncodeModels(decoded.entries) == content, "re-encoding is byte-identical");
		return ok;
	}

	auto ExpectCorruptFilesFailClosed() -> bool {
		const auto valid = EncodeModels(std::vector{ModelEntry(0)});
		bool       ok    = true;
		ok &= ExpectDecodeFailure("", UserModelStatus::kParseError, "User model file is truncated",
		                          "empty file");
		ok &= ExpectDecodeFailure(valid.substr(0, valid.size() / 2), UserModelStatus::kParseError,
		                          "User model file checksum does not match; the file is corrupted",
		                          "truncated file");
		auto bad_magic    = valid;
		bad_magic.front() = 'X';
		ok &= ExpectDecodeFailure(bad_magic, UserModelStatus::kParseError,
		                          "File is not a Howdy user model file", "bad magic");
		ok &= ExpectDecodeFailure(R"([{"id":0,"label":"legacy","data":[[1.0]]}])",
		                          UserModelStatus::kParseError,
		                          "File is not a Howdy user model file", "3.x JSON content");
		auto version  = valid;
		version.at(8) = '\x02';
		ok &= ExpectDecodeFailure(version, UserModelStatus::kParseError,
		                          "User model file format version is not supported",
		                          "unknown version");
		ok &= ExpectDecodeFailure(
		    howdy::test::user_model_codec::CorruptMiddleByte(valid), UserModelStatus::kParseError,
		    "User model file checksum does not match; the file is corrupted", "flipped byte");
		ok &= ExpectDecodeFailure(valid + '\0', UserModelStatus::kParseError,
		                          "User model file checksum does not match; the file is corrupted",
		                          "trailing byte");
		ok &= ExpectDecodeFailure(std::string(codec::kMaxFileBytes + 1, '\0'),
		                          UserModelStatus::kOversized,
		                          "User model file exceeds safety limit", "oversized file");
		return ok;
	}

	auto ExpectEncodeRejectsInvalidEntries() -> bool {
		bool ok = true;
		ok &= ExpectEncodeFailure({}, UserModelStatus::kInvalidShape,
		                          "User model file contains no models", "empty list");
		ok &= ExpectEncodeFailure({ModelEntry(-1)}, UserModelStatus::kInvalidShape,
		                          "Stored face model ID must not be negative", "negative id");
		ok &= ExpectEncodeFailure({ModelEntry(std::numeric_limits<int>::max())},
		                          UserModelStatus::kInvalidShape,
		                          howdy::native::kStoredModelIdLimitMessage, "id at INT_MAX");
		ok &= ExpectEncodeFailure({ModelEntry(1), ModelEntry(1)}, UserModelStatus::kInvalidShape,
		                          "Stored face model IDs must be unique and ascending",
		                          "duplicate id");
		ok &= ExpectEncodeFailure({ModelEntry(2), ModelEntry(1)}, UserModelStatus::kInvalidShape,
		                          "Stored face model IDs must be unique and ascending",
		                          "descending ids");
		ok &= ExpectEncodeFailure({ModelEntry(0, "Model", {})}, UserModelStatus::kInvalidShape,
		                          "Stored face model contains no encodings", "no encodings");
		const std::vector<std::vector<float>> too_many(codec::kMaxEncodingsPerModel + 1, {0.1F});
		ok &=
		    ExpectEncodeFailure({ModelEntry(0, "Model", too_many)}, UserModelStatus::kOversized,
			                    howdy::native::kStoredEncodingsLimitMessage, "too many encodings");
		ok &= ExpectEncodeFailure(
		    {ModelEntry(0, "Model", {std::vector<float>(codec::kMaxEncodingLength + 1, 0.1F)})},
		    UserModelStatus::kOversized, "Stored face encoding length is outside the safety limit",
		    "encoding too long");
		ok &= ExpectEncodeFailure({ModelEntry(0, "Model", {{}})}, UserModelStatus::kOversized,
		                          "Stored face encoding length is outside the safety limit",
		                          "empty encoding");
		ok &= ExpectEncodeFailure({ModelEntry(0, "Model", {{0.1F}, {0.1F, 0.2F}})},
		                          UserModelStatus::kInvalidShape,
		                          "Stored face encodings differ in length", "mixed lengths");
		constexpr std::array non_finite = {
		    std::numeric_limits<float>::quiet_NaN(),
		    std::numeric_limits<float>::infinity(),
		    -std::numeric_limits<float>::infinity(),
		};
		for (const float bad : non_finite) {
			ok &= ExpectEncodeFailure(
			    {ModelEntry(0, "Model", {{0.1F, bad}})}, UserModelStatus::kInvalidShape,
			    "Stored face encoding contains an invalid value", "non-finite value");
		}
		ok &= ExpectEncodeFailure({ModelEntry(0, "bad/label")}, UserModelStatus::kInvalidShape,
		                          "Stored face model label is invalid", "unsafe label");
		ok &= ExpectEncodeFailure({ModelEntry(0, "")}, UserModelStatus::kInvalidShape,
		                          "Stored face model label is invalid", "empty label");
		auto no_backend    = ModelEntry(0);
		no_backend.backend = "";
		ok &= ExpectEncodeFailure({no_backend}, UserModelStatus::kInvalidShape,
		                          "Stored face model metadata is invalid", "empty backend");
		auto spaced_model  = ModelEntry(0);
		spaced_model.model = "sface model.onnx";
		ok &= ExpectEncodeFailure({spaced_model}, UserModelStatus::kInvalidShape,
		                          "Stored face model metadata is invalid", "unsafe model name");
		return ok;
	}

	auto ExpectModelListLimits() -> bool {
		std::vector<UserModelEntry> entries;
		entries.reserve(codec::kMaxModels + 1);
		for (std::size_t index = 0; index < codec::kMaxModels; ++index) {
			entries.push_back(ModelEntry(static_cast<int>(index)));
		}
		const auto at_limit = codec::Decode(EncodeModels(entries));
		bool       ok       = Expect(at_limit.status == UserModelStatus::kOk &&
		                                 at_limit.entries.size() == codec::kMaxModels,
		                             "model count at the limit round-trips");
		entries.push_back(ModelEntry(static_cast<int>(codec::kMaxModels)));
		ok &= ExpectEncodeFailure(entries, UserModelStatus::kOversized,
		                          howdy::native::kStoredModelListLimitMessage,
		                          "model count over the limit");

		const std::vector<std::vector<float>> at_limit_encodings(
		    codec::kMaxEncodingsPerModel, std::vector<float>(codec::kMaxEncodingLength, 0.5F));
		const auto max_encodings =
		    codec::Decode(EncodeModels(std::vector{ModelEntry(0, "Model", at_limit_encodings)}));
		ok &= Expect(max_encodings.status == UserModelStatus::kOk &&
		                 max_encodings.entries.front().encodings.size() ==
		                     codec::kMaxEncodingsPerModel,
		             "encoding count and length at the limit round-trip");
		return ok;
	}

	auto ExpectLabelRules() -> bool {
		bool ok = Expect(codec::kMaxLabelBytes == 24, "label limit is 24 bytes");
		ok &= Expect(codec::IsValidModelLabel("Laptop camera"), "normal label is valid");
		ok &= Expect(codec::IsValidModelLabel("\xE0\xB8\x81\xE0\xB8\xA5\xE0\xB9\x89\xE0\xB8\xAD"),
		             "multibyte label is valid");
		ok &= Expect(codec::IsValidModelLabel(std::string(codec::kMaxLabelBytes, 'x')),
		             "label at the limit is valid");
		ok &= Expect(!codec::IsValidModelLabel(std::string(codec::kMaxLabelBytes + 1, 'x')),
		             "label over the limit is invalid");
		ok &= Expect(!codec::IsValidModelLabel(""), "empty label is invalid");
		const std::array<std::string_view, 13> unsafe_labels = {
		    "bad/name",
		    "bad\\name",
		    "bad\nname",
		    "bad\tname",
		    "bad\x7Fname",
		    "bad\xC2\x9Bname",
		    "ab\xE0\xB8",
		    "\xFF",
		    "\xC0\x80",
		    "\xED\xA0\x80",
		    "\xF4\x90\x80\x80",
		    "\xE0\xB8\x41",
		    std::string_view("nul\0byte", 8),
		};
		for (const auto bad : unsafe_labels) {
			ok &= Expect(!codec::IsValidModelLabel(bad), "unsafe label is invalid");
		}

		ok &= Expect(codec::ModelLabelPrefixLength("short") == 5, "short text is kept whole");
		const auto ascii = std::string(codec::kMaxLabelBytes + 4, 'y');
		ok &= Expect(codec::ModelLabelPrefixLength(ascii) == codec::kMaxLabelBytes,
		             "long ASCII text is cut at the limit");
		// 23 ASCII bytes, then a 3-byte Thai character straddling the limit.
		const auto straddle = std::string(codec::kMaxLabelBytes - 1, 'x') + "\xE0\xB8\x81" + "tail";
		ok &= Expect(codec::ModelLabelPrefixLength(straddle) == codec::kMaxLabelBytes - 1,
		             "cut does not split a code point");
		return ok;
	}

	auto ExpectExportedLimits() -> bool {
		bool ok = Expect(codec::kMaxFileBytes == std::size_t{1024} * 1024, "file limit is 1 MiB");
		ok &= Expect(codec::kMaxModels == 256, "model limit is 256");
		ok &= Expect(codec::kMaxEncodingsPerModel == 32, "encoding count limit is 32");
		ok &= Expect(codec::kMaxEncodingLength == 1024, "encoding length limit is 1024");
		return ok;
	}

}  // namespace

auto main() -> int {
	try {
		bool ok = true;
		ok &= ExpectRoundTrip();
		ok &= ExpectCorruptFilesFailClosed();
		ok &= ExpectEncodeRejectsInvalidEntries();
		ok &= ExpectModelListLimits();
		ok &= ExpectLabelRules();
		ok &= ExpectExportedLimits();
		return ok ? 0 : 1;
	} catch (const std::exception &error) {
		std::cerr << "FAIL: unexpected exception: " << error.what() << "\n";
		return 1;
	} catch (...) {
		std::cerr << "FAIL: unexpected non-standard exception\n";
		return 1;
	}
}
