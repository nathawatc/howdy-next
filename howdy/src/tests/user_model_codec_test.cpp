#include "storage/user_model_codec.hpp"
#include "storage/user_model_limits.hpp"

#include <cstddef>
#include <exception>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

	constexpr auto kBackend = "opencv_dnn_sface";
	constexpr auto kMetric  = "cosine";
	constexpr auto kModel   = "sface.onnx";

	auto expect(bool condition, const std::string &message) -> bool {
		if (!condition) {
			std::cerr << "FAIL: " << message << "\n";
			return false;
		}
		return true;
	}

	auto expect_status(howdy::native::UserModelStatus actual,
	                   howdy::native::UserModelStatus expected, const std::string &message)
	    -> bool {
		return expect(actual == expected, message);
	}

	auto model_json(std::string_view id = "7", std::string_view time = "1700000000",
	                std::string_view label   = R"("Office camera")",
	                std::string_view backend = R"("opencv_dnn_sface")",
	                std::string_view metric  = R"("cosine")",
	                std::string_view model   = R"("sface.onnx")",
	                std::string_view data = "[[1.0,2.0,-3.5]]", std::string_view extra = {})
	    -> std::string {
		return "{\"id\":" + std::string(id) + ",\"time\":" + std::string(time) +
		       ",\"label\":" + std::string(label) + ",\"backend\":" + std::string(backend) +
		       ",\"metric\":" + std::string(metric) + ",\"model\":" + std::string(model) +
		       ",\"data\":" + std::string(data) + std::string(extra) + "}";
	}

	auto model_list(const std::vector<std::string> &models) -> std::string {
		std::string output = "[";
		for (std::size_t index = 0; index < models.size(); ++index) {
			if (index > 0) {
				output += ',';
			}
			output += models[index];
		}
		output += ']';
		return output;
	}

	auto decode(std::string_view content, bool strict_shape = true)
	    -> howdy::native::user_model_codec::Document {
		return howdy::native::user_model_codec::decode_document(content, kBackend, kMetric, kModel,
		                                                        strict_shape);
	}

	auto expect_valid_strict_document() -> bool {
		using howdy::native::UserModelStatus;

		bool       ok       = true;
		const auto document = decode(model_list({model_json()}));
		ok &= expect_status(document.result.status, UserModelStatus::kOk,
		                    "valid strict document returns kOk");
		ok &= expect(document.result.entries.size() == 1, "valid strict document has one entry");
		ok &= expect(document.result.next_id == 8, "valid strict document reports next_id");
		if (!document.result.entries.empty()) {
			const auto &entry = document.result.entries.front();
			ok &= expect(entry.id == 7, "valid strict document preserves id");
			ok &= expect(entry.time == 1700000000LL, "valid strict document preserves time");
			ok &= expect(entry.label == "Office camera", "valid strict document preserves label");
			ok &= expect(entry.backend == kBackend, "valid strict document preserves backend");
			ok &= expect(entry.metric == kMetric, "valid strict document preserves metric");
			ok &= expect(entry.model == kModel, "valid strict document preserves model");
			ok &= expect(entry.encodings == std::vector<std::vector<float>>{{1.0F, 2.0F, -3.5F}},
			             "valid strict document preserves encoding values");
		}
		return ok;
	}

	auto expect_new_entry_round_trip() -> bool {
		using howdy::native::UserModelEntry;
		using howdy::native::UserModelStatus;

		bool                 ok = true;
		const UserModelEntry entry{
		    .id        = 12,
		    .time      = 1700000123LL,
		    .label     = "roundtrip",
		    .backend   = kBackend,
		    .metric    = kMetric,
		    .model     = kModel,
		    .encodings = {{1.0F, 2.0F}, {3.5F, -4.25F}},
		};
		howdy::native::user_model_codec::Document document;
		ok &= expect(howdy::native::user_model_codec::append_entry(document, entry),
		             "new document accepts entry");
		ok &= expect(!howdy::native::user_model_codec::is_empty(document),
		             "new document is non-empty after append");
		const auto content = howdy::native::user_model_codec::serialize_document(document);
		ok &= expect(content.has_value(), "new document serializes");
		if (!content.has_value()) {
			return false;
		}
		const auto decoded = decode(*content);
		ok &= expect_status(decoded.result.status, UserModelStatus::kOk,
		                    "round trip document returns kOk");
		ok &= expect(decoded.result.entries.size() == 1, "round trip document has one entry");
		ok &= expect(decoded.result.next_id == 13, "round trip document reports next_id");
		if (!decoded.result.entries.empty()) {
			const auto &actual = decoded.result.entries.front();
			ok &= expect(actual.id == entry.id, "round trip preserves id");
			ok &= expect(actual.time == entry.time, "round trip preserves time");
			ok &= expect(actual.label == entry.label, "round trip preserves label");
			ok &= expect(actual.backend == entry.backend, "round trip preserves backend");
			ok &= expect(actual.metric == entry.metric, "round trip preserves metric");
			ok &= expect(actual.model == entry.model, "round trip preserves model");
			ok &=
			    expect(actual.encodings == entry.encodings, "round trip preserves float encodings");
		}
		return ok;
	}

	auto expect_strict_parser_behavior() -> bool {
		using howdy::native::UserModelStatus;

		bool ok = true;
		ok &= expect_status(decode("[").result.status, UserModelStatus::kParseError,
		                    "malformed JSON returns kParseError");
		const auto malformed = decode("[");
		ok &= expect(malformed.result.error_message == "Failed to parse user model JSON",
		             "parser failures use stable Howdy message");
		ok &= expect_status(decode("{}").result.status, UserModelStatus::kInvalidShape,
		                    "non-array root returns kInvalidShape");
		ok &= expect_status(decode("[]").result.status, UserModelStatus::kNoModel,
		                    "empty array returns kNoModel");

		const std::string bom("\xEF\xBB\xBF", 3);
		ok &= expect_status(decode(bom + model_list({model_json()})).result.status,
		                    UserModelStatus::kOk, "UTF-8 BOM is accepted");
		ok &= expect_status(decode("[/*comment*/" + model_json() + "]").result.status,
		                    UserModelStatus::kParseError, "comments are rejected");
		ok &= expect_status(decode("[" + model_json() + ",]").result.status,
		                    UserModelStatus::kParseError, "trailing comma is rejected");
		ok &= expect_status(decode(model_list({model_json()}) + "[]").result.status,
		                    UserModelStatus::kParseError, "second JSON document is rejected");
		ok &= expect_status(decode(model_list({model_json()}) + "junk").result.status,
		                    UserModelStatus::kParseError, "trailing junk is rejected");

		std::string invalid_utf8   = model_list({model_json()});
		const auto  label_offset   = invalid_utf8.find("Office camera");
		invalid_utf8[label_offset] = static_cast<char>(0xFF);
		ok &= expect_status(decode(invalid_utf8).result.status, UserModelStatus::kParseError,
		                    "invalid UTF-8 is rejected");
		for (const std::string_view number : {"NaN", "Infinity", "1e999"}) {
			ok &= expect_status(
			    decode(model_list({model_json(
			               "7", "1700000000", R"("Office camera")", R"("opencv_dnn_sface")",
			               R"("cosine")", R"("sface.onnx")", "[[" + std::string(number) + "]]")}))
			        .result.status,
			    UserModelStatus::kParseError, std::string(number) + " encoding is rejected");
		}
		return ok;
	}

	auto expect_duplicate_key_behavior() -> bool {
		using howdy::native::UserModelStatus;

		bool ok = true;
		ok &= expect_status(
		    decode(model_list(
		               {model_json("7", "1700000000", R"("Office camera")", R"("opencv_dnn_sface")",
		                           R"("cosine")", R"("sface.onnx")", "[[1.0]]", ",\"id\":8")}))
		        .result.status,
		    UserModelStatus::kInvalidShape, "duplicate known direct key is rejected");
		ok &= expect_status(
		    decode(model_list({model_json("7", "1700000000", R"("Office camera")",
		                                  R"("opencv_dnn_sface")", R"("cosine")", R"("sface.onnx")",
		                                  "[[1.0]]", R"(,"future":1,"future":2)")}))
		        .result.status,
		    UserModelStatus::kInvalidShape, "duplicate unknown direct key is rejected");
		const auto tolerant_duplicate = decode(
		    model_list({model_json("7", "1700000000", R"("Office camera")", R"("opencv_dnn_sface")",
		                           R"("cosine")", R"("sface.onnx")", "[[1.0]]", ",\"id\":8")}),
		    false);
		ok &= expect_status(tolerant_duplicate.result.status, UserModelStatus::kOk,
		                    "duplicate direct key is tolerated without strict shape");
		ok &= expect(tolerant_duplicate.result.entries.size() == 1,
		             "tolerant duplicate key preserves one entry");
		ok &= expect(tolerant_duplicate.result.entries.size() == 1 &&
		                 tolerant_duplicate.result.entries.front().id == 8,
		             "tolerant duplicate key keeps the last ID");
		const auto tolerant_known_fields = decode(
		    model_list({model_json("7", "1700000000", R"("Office camera")", R"("opencv_dnn_sface")",
		                           R"("cosine")", R"("sface.onnx")", "[[1.0]]",
		                           R"(,"label":"Backup camera","data":[[4.0,5.0]])")}),
		    false);
		ok &= expect_status(tolerant_known_fields.result.status, UserModelStatus::kOk,
		                    "duplicate label and data are tolerated without strict shape");
		ok &= expect(tolerant_known_fields.result.entries.size() == 1 &&
		                 tolerant_known_fields.result.entries.front().label == "Backup camera",
		             "tolerant duplicate label keeps the last value");
		ok &= expect(tolerant_known_fields.result.entries.size() == 1 &&
		                 tolerant_known_fields.result.entries.front().encodings ==
		                     std::vector<std::vector<float>>{{4.0F, 5.0F}},
		             "tolerant duplicate data keeps the last value");
		ok &= expect_status(
		    decode(model_list({model_json("7", "1700000000", R"("Office camera")",
		                                  R"("opencv_dnn_sface")", R"("cosine")", R"("sface.onnx")",
		                                  "[[1.0]]", R"(,"future":{"revision":1,"revision":2})")}))
		        .result.status,
		    UserModelStatus::kOk, "nested unknown duplicate key is not recursively rejected");
		return ok;
	}

	auto expect_immutable_to_mutable_conversion() -> bool {
		using howdy::native::UserModelEntry;
		using howdy::native::UserModelStatus;

		auto                 document = decode(model_list({model_json("0")}));
		const UserModelEntry appended{
		    .id        = 1,
		    .time      = 1700000001,
		    .label     = "converted",
		    .backend   = kBackend,
		    .metric    = kMetric,
		    .model     = kModel,
		    .encodings = {{4.0F, 5.0F}},
		};
		bool ok = expect_status(document.result.status, UserModelStatus::kOk,
		                        "immutable conversion fixture decodes");
		ok &= expect(howdy::native::user_model_codec::append_entry(document, appended),
		             "immutable document converts and appends");
		ok &= expect(howdy::native::user_model_codec::erase_entry(document, 0),
		             "converted document remains mutable");
		const auto serialized = howdy::native::user_model_codec::serialize_document(document);
		ok &= expect(serialized.has_value(), "converted document serializes");
		if (serialized.has_value()) {
			const auto round_trip = decode(*serialized);
			ok &= expect_status(round_trip.result.status, UserModelStatus::kOk,
			                    "converted document round trip returns kOk");
			ok &= expect(round_trip.result.entries.size() == 1 &&
			                 round_trip.result.entries.front().id == 1,
			             "converted document keeps mutation results");
		}
		return ok;
	}

	auto expect_malformed_scalar_fields() -> bool {
		using howdy::native::UserModelStatus;

		bool ok = true;
		ok &=
		    expect_status(decode(model_list({model_json(R"("7")")})).result.status,
		                  UserModelStatus::kInvalidShape, "string model id returns kInvalidShape");
		ok &= expect_status(decode(model_list({model_json("7.0")})).result.status,
		                    UserModelStatus::kInvalidShape, "real model id returns kInvalidShape");
		ok &= expect_status(decode(model_list({model_json("2147483648")})).result.status,
		                    UserModelStatus::kInvalidShape, "out-of-range model id is rejected");
		ok &= expect_status(decode(model_list({model_json("7", R"("bad")")})).result.status,
		                    UserModelStatus::kInvalidShape,
		                    "string model time returns kInvalidShape");
		ok &=
		    expect_status(decode(model_list({model_json("7", "1.5")})).result.status,
		                  UserModelStatus::kInvalidShape, "real model time returns kInvalidShape");
		ok &= expect_status(
		    decode(model_list({model_json("7", "18446744073709551615")})).result.status,
		    UserModelStatus::kInvalidShape, "out-of-range model time is rejected");
		ok &= expect_status(decode(model_list({model_json("7", "1700000000", "7")})).result.status,
		                    UserModelStatus::kInvalidShape,
		                    "non-string model label returns kInvalidShape");
		ok &= expect_status(
		    decode(model_list({model_json("7", "1700000000", R"("bad/name")")})).result.status,
		    UserModelStatus::kParseError, "unsafe model label returns kParseError");
		return ok;
	}

	auto expect_strict_shape_contract() -> bool {
		using howdy::native::UserModelStatus;

		bool              ok = true;
		const auto *const missing_data =
		    R"([{"id":7,"time":1700000000,"label":"Office camera","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx"}])";
		ok &=
		    expect_status(decode(missing_data, true).result.status, UserModelStatus::kInvalidShape,
		                  "missing data with strict shape returns kInvalidShape");
		const auto tolerant_missing_data = decode(missing_data, false);
		ok &= expect_status(tolerant_missing_data.result.status, UserModelStatus::kOk,
		                    "missing data without strict shape returns kOk");
		ok &= expect(tolerant_missing_data.result.entries.size() == 1 &&
		                 tolerant_missing_data.result.entries.front().encodings.empty(),
		             "missing data without strict shape preserves entry with zero encodings");

		const auto non_array_encoding =
		    model_list({model_json("7", "1700000000", R"("Office camera")", R"("opencv_dnn_sface")",
		                           R"("cosine")", R"("sface.onnx")", R"([[1.0],"bad",[2.0]])")});
		const auto tolerant_non_array = decode(non_array_encoding, false);
		ok &= expect_status(tolerant_non_array.result.status, UserModelStatus::kOk,
		                    "non-array encoding without strict shape returns kOk");
		ok &= expect(tolerant_non_array.result.entries.size() == 1 &&
		                 tolerant_non_array.result.entries.front().encodings.size() == 2,
		             "non-array encoding without strict shape is skipped");
		ok &= expect_status(decode(non_array_encoding, true).result.status,
		                    UserModelStatus::kInvalidShape,
		                    "non-array encoding with strict shape returns kInvalidShape");

		const auto *const legacy_missing_id =
		    R"([{"label":"legacy","backend":"opencv_dnn_sface","data":[[1.0]]}])";
		const auto tolerant_missing_id = decode(legacy_missing_id, false);
		ok &= expect_status(tolerant_missing_id.result.status, UserModelStatus::kOk,
		                    "missing ID without strict shape returns kOk");
		ok &= expect(tolerant_missing_id.result.entries.front().id == -1,
		             "missing ID without strict shape preserves legacy sentinel");
		return ok;
	}

	auto expect_compatibility_checks() -> bool {
		using howdy::native::UserModelStatus;

		bool ok = true;
		ok &= expect_status(decode(model_list({model_json("7", "1700000000", R"("Office camera")",
		                                                  R"("other_backend")")}))
		                        .result.status,
		                    UserModelStatus::kIncompatibleBackend,
		                    "backend mismatch returns kIncompatibleBackend");
		ok &= expect_status(decode(model_list({model_json("7", "1700000000", R"("Office camera")",
		                                                  R"("opencv_dnn_sface")", R"("l2")")}))
		                        .result.status,
		                    UserModelStatus::kIncompatibleMetric,
		                    "metric mismatch returns kIncompatibleMetric");
		ok &= expect_status(decode(model_list({model_json("7", "1700000000", R"("Office camera")",
		                                                  R"("opencv_dnn_sface")", R"("cosine")",
		                                                  R"("other.onnx")")}))
		                        .result.status,
		                    UserModelStatus::kIncompatibleModel,
		                    "model mismatch returns kIncompatibleModel");
		return ok;
	}

	auto expect_encoding_validation() -> bool {
		using howdy::native::UserModelStatus;
		using howdy::native::user_model_limits::kMaxEncodingLength;

		bool ok = true;
		ok &= expect_status(howdy::native::user_model_codec::validate_encoding({}).status,
		                    UserModelStatus::kOversized, "empty encoding returns kOversized");
		ok &= expect_status(
		    howdy::native::user_model_codec::validate_encoding({0.0F, 1.0F, -2.5F}).status,
		    UserModelStatus::kOk, "finite valid encoding returns kOk");
		ok &= expect_status(howdy::native::user_model_codec::validate_encoding(
		                        {std::numeric_limits<float>::quiet_NaN()})
		                        .status,
		                    UserModelStatus::kInvalidShape, "NaN encoding returns kInvalidShape");
		ok &= expect_status(howdy::native::user_model_codec::validate_encoding(
		                        {std::numeric_limits<float>::infinity()})
		                        .status,
		                    UserModelStatus::kInvalidShape,
		                    "infinity encoding returns kInvalidShape");
		ok &= expect_status(howdy::native::user_model_codec::validate_encoding(
		                        std::vector<float>(kMaxEncodingLength + 1, 0.0F))
		                        .status,
		                    UserModelStatus::kOversized,
		                    "oversized encoding length returns kOversized");
		return ok;
	}

	auto repeated_array(std::size_t count, std::string_view value) -> std::string {
		std::string output = "[";
		for (std::size_t index = 0; index < count; ++index) {
			if (index > 0) {
				output += ',';
			}
			output += value;
		}
		output += ']';
		return output;
	}

	auto nested_array(std::size_t depth) -> std::string {
		return std::string(depth, '[') + "0" + std::string(depth, ']');
	}

	auto expect_boundaries_and_limits() -> bool {
		using howdy::native::UserModelStatus;
		using howdy::native::user_model_limits::kMaxEncodingLength;
		using howdy::native::user_model_limits::kMaxEncodingsPerModel;
		using howdy::native::user_model_limits::kMaxStoredModels;

		bool       ok                    = true;
		const auto max_encoding          = repeated_array(kMaxEncodingLength, "0.0");
		const auto max_encoding_document = decode(
		    model_list({model_json("7", "1700000000", R"("Office camera")", R"("opencv_dnn_sface")",
		                           R"("cosine")", R"("sface.onnx")", "[" + max_encoding + "]")}));
		ok &= expect_status(max_encoding_document.result.status, UserModelStatus::kOk,
		                    "max encoding length returns kOk");
		ok &= expect(max_encoding_document.result.entries.front().encodings.front().size() ==
		                 kMaxEncodingLength,
		             "max encoding length is preserved");

		const auto max_encodings          = repeated_array(kMaxEncodingsPerModel, "[1.0]");
		const auto max_encodings_document = decode(
		    model_list({model_json("7", "1700000000", R"("Office camera")", R"("opencv_dnn_sface")",
		                           R"("cosine")", R"("sface.onnx")", max_encodings)}));
		ok &= expect_status(max_encodings_document.result.status, UserModelStatus::kOk,
		                    "max encodings per model returns kOk");
		ok &= expect(max_encodings_document.result.entries.front().encodings.size() ==
		                 kMaxEncodingsPerModel,
		             "max encodings per model are preserved");

		std::vector<std::string> max_models;
		max_models.reserve(kMaxStoredModels);
		for (std::size_t index = 0; index < kMaxStoredModels; ++index) {
			max_models.push_back(model_json(std::to_string(index)));
		}
		const auto max_models_document = decode(model_list(max_models));
		ok &= expect_status(max_models_document.result.status, UserModelStatus::kOk,
		                    "max stored models returns kOk");
		ok &= expect(max_models_document.result.entries.size() == kMaxStoredModels,
		             "max stored models are preserved");
		ok &= expect(std::cmp_equal(max_models_document.result.next_id, kMaxStoredModels),
		             "max stored models reports next_id");

		ok &= expect_status(
		    decode(
		        model_list({model_json("7", "1700000000", R"("Office camera")",
		                               R"("opencv_dnn_sface")", R"("cosine")", R"("sface.onnx")",
		                               "[" + repeated_array(kMaxEncodingLength + 1, "0.0") + "]")}))
		        .result.status,
		    UserModelStatus::kOversized, "oversized encoding length returns kOversized");
		ok &= expect_status(
		    decode(model_list({model_json("7", "1700000000", R"("Office camera")",
		                                  R"("opencv_dnn_sface")", R"("cosine")", R"("sface.onnx")",
		                                  repeated_array(kMaxEncodingsPerModel + 1, "[1.0]"))}))
		        .result.status,
		    UserModelStatus::kOversized, "too many encodings per model returns kOversized");
		max_models.push_back(model_json(std::to_string(kMaxStoredModels)));
		ok &=
		    expect_status(decode(model_list(max_models)).result.status, UserModelStatus::kOversized,
		                  "too many stored models returns kOversized");
		return ok;
	}

	auto expect_multi_model_behavior() -> bool {
		using howdy::native::UserModelStatus;

		bool       ok       = true;
		const auto document = decode(model_list({
		    model_json("10", "1700000010", R"("highest")", R"("opencv_dnn_sface")", R"("cosine")",
		               R"("sface.onnx")", "[[10.0]]"),
		    model_json("2", "1700000002", R"("lowest")", R"("opencv_dnn_sface")", R"("cosine")",
		               R"("sface.onnx")", "[[2.0,2.5]]"),
		    model_json("7", "1700000007", R"("middle")", R"("opencv_dnn_sface")", R"("cosine")",
		               R"("sface.onnx")", "[[7.0]]"),
		}));
		ok &= expect_status(document.result.status, UserModelStatus::kOk,
		                    "out-of-order non-contiguous model IDs return kOk");
		ok &= expect(document.result.entries.size() == 3,
		             "out-of-order non-contiguous model IDs preserve entry count");
		ok &= expect(document.result.next_id == 11,
		             "out-of-order non-contiguous model IDs report max id plus one");
		if (document.result.entries.size() == 3) {
			ok &= expect(document.result.entries[0].id == 10 &&
			                 document.result.entries[0].label == "highest" &&
			                 document.result.entries[0].time == 1700000010LL &&
			                 document.result.entries[0].encodings ==
			                     std::vector<std::vector<float>>{{10.0F}},
			             "first out-of-order entry is preserved");
			ok &= expect(document.result.entries[1].id == 2 &&
			                 document.result.entries[1].label == "lowest" &&
			                 document.result.entries[1].time == 1700000002LL &&
			                 document.result.entries[1].encodings ==
			                     std::vector<std::vector<float>>{{2.0F, 2.5F}},
			             "second out-of-order entry is preserved");
			ok &= expect(document.result.entries[2].id == 7 &&
			                 document.result.entries[2].label == "middle" &&
			                 document.result.entries[2].time == 1700000007LL &&
			                 document.result.entries[2].encodings ==
			                     std::vector<std::vector<float>>{{7.0F}},
			             "third out-of-order entry is preserved");
		}
		ok &= expect_status(decode(model_list({model_json("3"), model_json("3")})).result.status,
		                    UserModelStatus::kInvalidShape,
		                    "duplicate strict IDs return kInvalidShape");
		ok &= expect_status(decode(model_list({model_json("2147483647")})).result.status,
		                    UserModelStatus::kInvalidShape, "INT_MAX ID returns kInvalidShape");
		return ok;
	}

	auto expect_unknown_field_preservation() -> bool {
		using howdy::native::UserModelEntry;
		using howdy::native::UserModelStatus;

		bool       ok       = true;
		const auto original = model_list({
		    model_json("0", "1", R"("first")", R"("opencv_dnn_sface")", R"("cosine")",
		               R"("sface.onnx")", "[[0.1,0.2]]",
		               R"(,"future_field":{"revision":2,"weight":0.123456789})"),
		    model_json("1", "2", R"("second")", R"("opencv_dnn_sface")", R"("cosine")",
		               R"("sface.onnx")", "[[0.3,0.4]]"),
		});

		auto                 append_document = decode(original);
		const UserModelEntry appended{
		    .id        = 2,
		    .time      = 3,
		    .label     = "third",
		    .backend   = kBackend,
		    .metric    = kMetric,
		    .model     = kModel,
		    .encodings = {{0.5F, 0.6F}},
		};
		ok &= expect_status(append_document.result.status, UserModelStatus::kOk,
		                    "unknown-field append fixture decodes");
		ok &= expect(howdy::native::user_model_codec::append_entry(append_document, appended),
		             "append mutates copied document");
		const auto appended_json =
		    howdy::native::user_model_codec::serialize_document(append_document);
		ok &= expect(appended_json.has_value() && appended_json->contains("\"future_field\"") &&
		                 appended_json->contains("\"revision\":2") &&
		                 appended_json->contains("0.123456789"),
		             "append preserves unknown nested field and real value");

		auto remove_document = decode(original);
		ok &= expect(howdy::native::user_model_codec::erase_entry(remove_document, 1),
		             "remove mutates copied document");
		const auto removed_json =
		    howdy::native::user_model_codec::serialize_document(remove_document);
		ok &= expect(removed_json.has_value() && removed_json->contains("\"future_field\"") &&
		                 removed_json->contains("\"revision\":2") &&
		                 removed_json->contains("0.123456789"),
		             "removing another entry preserves unknown field");
		return ok;
	}

	auto expect_json_depth_limit() -> bool {
		using howdy::native::UserModelStatus;
		using howdy::native::user_model_limits::kMaxJsonNestingDepth;

		const auto at_limit = decode(model_list({model_json(
		    "0", "1", R"("deep")", R"("opencv_dnn_sface")", R"("cosine")", R"("sface.onnx")",
		    "[[0.1]]", ",\"unknown\":" + nested_array(kMaxJsonNestingDepth - 2))}));
		bool       ok       = expect_status(at_limit.result.status, UserModelStatus::kOk,
		                                    "container nesting exactly at limit returns kOk");

		const auto over_limit = decode(model_list({model_json(
		    "0", "1", R"("deep")", R"("opencv_dnn_sface")", R"("cosine")", R"("sface.onnx")",
		    "[[0.1]]", ",\"unknown\":" + nested_array(kMaxJsonNestingDepth - 1))}));
		ok &= expect_status(over_limit.result.status, UserModelStatus::kOversized,
		                    "one container beyond nesting limit returns kOversized");
		ok &= expect(over_limit.result.error_message ==
		                 "User model JSON nesting exceeds safety limit",
		             "nesting limit uses stable error message");
		return ok;
	}

}  // namespace

auto main() -> int {
	try {
		bool ok = true;
		ok &= expect_valid_strict_document();
		ok &= expect_new_entry_round_trip();
		ok &= expect_strict_parser_behavior();
		ok &= expect_duplicate_key_behavior();
		ok &= expect_immutable_to_mutable_conversion();
		ok &= expect_malformed_scalar_fields();
		ok &= expect_strict_shape_contract();
		ok &= expect_compatibility_checks();
		ok &= expect_encoding_validation();
		ok &= expect_boundaries_and_limits();
		ok &= expect_multi_model_behavior();
		ok &= expect_unknown_field_preservation();
		ok &= expect_json_depth_limit();
		return ok ? 0 : 1;
	} catch (const std::exception &error) {
		std::cerr << "FAIL: unexpected exception: " << error.what() << "\n";
		return 1;
	} catch (...) {
		std::cerr << "FAIL: unexpected non-standard exception\n";
		return 1;
	}
}
