#include "storage/user_model_codec.hpp"
#include "storage/user_model_limits.hpp"

#include <cstddef>
#include <exception>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

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

	auto decode(const std::string &content, bool strict_shape = true)
	    -> howdy::native::user_model_codec::Document {
		std::istringstream input(content);
		return howdy::native::user_model_codec::decode_document(input, kBackend, kMetric, kModel,
		                                                        strict_shape);
	}

	auto valid_model_json(int id = 7) -> nlohmann::json {
		return {
		    {"id", id},
		    {"time", 1700000000LL},
		    {"label", "Office camera"},
		    {"backend", kBackend},
		    {"metric", kMetric},
		    {"model", kModel},
		    {"data", nlohmann::json::array({nlohmann::json::array({1.0F, 2.0F, -3.5F})})},
		};
	}

	auto decode_models(const nlohmann::json &models, bool strict_shape = true)
	    -> howdy::native::user_model_codec::Document {
		return decode(models.dump(), strict_shape);
	}

	auto expect_status(howdy::native::UserModelStatus actual,
	                   howdy::native::UserModelStatus expected, const std::string &message)
	    -> bool {
		return expect(actual == expected, message);
	}

	auto expect_valid_strict_document() -> bool {
		using howdy::native::UserModelStatus;

		bool ok = true;

		const auto document = decode_models(nlohmann::json::array({valid_model_json()}));
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
			ok &= expect(entry.encodings.size() == 1, "valid strict document has one encoding");
			if (!entry.encodings.empty()) {
				ok &= expect(entry.encodings.front() == std::vector<float>({1.0F, 2.0F, -3.5F}),
				             "valid strict document preserves encoding values");
			}
		}

		return ok;
	}

	auto expect_encode_decode_round_trip() -> bool {
		using howdy::native::UserModelEntry;
		using howdy::native::UserModelStatus;

		bool ok = true;

		const UserModelEntry entry{
		    .id        = 12,
		    .time      = 1700000123LL,
		    .label     = "roundtrip",
		    .backend   = kBackend,
		    .metric    = kMetric,
		    .model     = kModel,
		    .encodings = {{1.0F, 2.0F}, {3.5F, -4.25F}},
		};
		const auto encoded  = howdy::native::user_model_codec::encode_entry(entry);
		const auto models   = nlohmann::json::array({encoded});
		const auto content  = howdy::native::user_model_codec::serialize_document(models);
		const auto document = decode(content);

		ok &= expect_status(document.result.status, UserModelStatus::kOk,
		                    "round trip document returns kOk");
		ok &= expect(document.result.entries.size() == 1, "round trip document has one entry");
		ok &= expect(document.result.next_id == 13, "round trip document reports next_id");
		if (!document.result.entries.empty()) {
			const auto &decoded = document.result.entries.front();
			ok &= expect(decoded.id == entry.id, "round trip preserves id");
			ok &= expect(decoded.time == entry.time, "round trip preserves time");
			ok &= expect(decoded.label == entry.label, "round trip preserves label");
			ok &= expect(decoded.backend == entry.backend, "round trip preserves backend");
			ok &= expect(decoded.metric == entry.metric, "round trip preserves metric");
			ok &= expect(decoded.model == entry.model, "round trip preserves model");
			ok &= expect(decoded.encodings == entry.encodings, "round trip preserves encodings");
		}

		return ok;
	}

	auto expect_invalid_documents() -> bool {
		using howdy::native::UserModelStatus;

		bool ok = true;

		ok &= expect_status(decode("[").result.status, UserModelStatus::kParseError,
		                    "malformed JSON returns kParseError");
		ok &= expect_status(decode("{}").result.status, UserModelStatus::kInvalidShape,
		                    "non-array root returns kInvalidShape");
		ok &= expect_status(
		    decode_models(nlohmann::json::array({valid_model_json(3), valid_model_json(3)}))
		        .result.status,
		    UserModelStatus::kInvalidShape, "duplicate strict IDs return kInvalidShape");
		ok &= expect_status(decode("[]").result.status, UserModelStatus::kNoModel,
		                    "empty array returns kNoModel");

		return ok;
	}

	auto expect_malformed_scalar_fields() -> bool {
		using howdy::native::UserModelStatus;

		bool ok = true;

		auto bad_id  = valid_model_json();
		bad_id["id"] = "7";
		ok &=
		    expect_status(decode_models(nlohmann::json::array({bad_id}), true).result.status,
		                  UserModelStatus::kInvalidShape, "string model id returns kInvalidShape");

		auto bad_time    = valid_model_json();
		bad_time["time"] = "bad";
		ok &= expect_status(decode_models(nlohmann::json::array({bad_time}), true).result.status,
		                    UserModelStatus::kInvalidShape,
		                    "string model time returns kInvalidShape");

		auto bad_label     = valid_model_json();
		bad_label["label"] = 7;
		ok &= expect_status(decode_models(nlohmann::json::array({bad_label}), true).result.status,
		                    UserModelStatus::kInvalidShape,
		                    "non-string model label returns kInvalidShape");

		auto unsafe_label     = valid_model_json();
		unsafe_label["label"] = "bad/name";
		ok &=
		    expect_status(decode_models(nlohmann::json::array({unsafe_label}), true).result.status,
		                  UserModelStatus::kParseError, "unsafe model label returns kParseError");

		return ok;
	}

	auto expect_strict_shape_contract() -> bool {
		using howdy::native::UserModelStatus;

		bool ok = true;

		auto missing_data = valid_model_json();
		missing_data.erase("data");
		ok &= expect_status(
		    decode_models(nlohmann::json::array({missing_data}), true).result.status,
		    UserModelStatus::kInvalidShape, "missing data with strict shape returns kInvalidShape");
		const auto tolerant_missing_data =
		    decode_models(nlohmann::json::array({missing_data}), false);
		ok &= expect_status(tolerant_missing_data.result.status, UserModelStatus::kOk,
		                    "missing data without strict shape returns kOk");
		ok &= expect(tolerant_missing_data.result.entries.size() == 1,
		             "missing data without strict shape preserves entry");
		if (!tolerant_missing_data.result.entries.empty()) {
			ok &= expect(tolerant_missing_data.result.entries.front().encodings.empty(),
			             "missing data without strict shape has zero encodings");
		}

		auto non_array_encoding    = valid_model_json();
		non_array_encoding["data"] = nlohmann::json::array(
		    {nlohmann::json::array({1.0F}), "bad encoding", nlohmann::json::array({2.0F})});
		const auto tolerant_non_array =
		    decode_models(nlohmann::json::array({non_array_encoding}), false);
		ok &= expect_status(tolerant_non_array.result.status, UserModelStatus::kOk,
		                    "non-array encoding without strict shape returns kOk");
		ok &= expect(tolerant_non_array.result.entries.size() == 1,
		             "non-array encoding without strict shape preserves entry");
		if (!tolerant_non_array.result.entries.empty()) {
			ok &= expect(tolerant_non_array.result.entries.front().encodings.size() == 2,
			             "non-array encoding without strict shape is skipped");
		}
		ok &= expect_status(
		    decode_models(nlohmann::json::array({non_array_encoding}), true).result.status,
		    UserModelStatus::kInvalidShape,
		    "non-array encoding with strict shape returns kInvalidShape");

		return ok;
	}

	auto expect_compatibility_checks() -> bool {
		using howdy::native::UserModelStatus;

		bool ok = true;

		auto backend_mismatch       = valid_model_json();
		backend_mismatch["backend"] = "other_backend";
		ok &= expect_status(decode_models(nlohmann::json::array({backend_mismatch})).result.status,
		                    UserModelStatus::kIncompatibleBackend,
		                    "backend mismatch returns kIncompatibleBackend");

		auto metric_mismatch      = valid_model_json();
		metric_mismatch["metric"] = "l2";
		ok &= expect_status(decode_models(nlohmann::json::array({metric_mismatch})).result.status,
		                    UserModelStatus::kIncompatibleMetric,
		                    "metric mismatch returns kIncompatibleMetric");

		auto model_mismatch     = valid_model_json();
		model_mismatch["model"] = "other.onnx";
		ok &= expect_status(decode_models(nlohmann::json::array({model_mismatch})).result.status,
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

	auto expect_accepted_boundaries() -> bool {
		using howdy::native::UserModelStatus;
		using howdy::native::user_model_limits::kMaxEncodingLength;
		using howdy::native::user_model_limits::kMaxEncodingsPerModel;
		using howdy::native::user_model_limits::kMaxStoredModels;

		bool ok = true;

		auto max_length_encoding = nlohmann::json::array();
		for (std::size_t index = 0; index < kMaxEncodingLength; ++index) {
			max_length_encoding.push_back(0.0F);
		}
		auto max_length_model    = valid_model_json();
		max_length_model["data"] = nlohmann::json::array({max_length_encoding});
		const auto max_length_document =
		    decode_models(nlohmann::json::array({max_length_model}), true);
		ok &= expect_status(max_length_document.result.status, UserModelStatus::kOk,
		                    "max encoding length returns kOk");
		ok &= expect(max_length_document.result.entries.size() == 1,
		             "max encoding length preserves entry");
		if (!max_length_document.result.entries.empty()) {
			const auto &entry = max_length_document.result.entries.front();
			ok &= expect(entry.encodings.size() == 1,
			             "max encoding length preserves exactly one encoding");
			if (!entry.encodings.empty()) {
				ok &= expect(entry.encodings.front().size() == kMaxEncodingLength,
				             "max encoding length is preserved");
			}
		}

		auto max_encodings_model    = valid_model_json();
		max_encodings_model["data"] = nlohmann::json::array();
		for (std::size_t index = 0; index < kMaxEncodingsPerModel; ++index) {
			max_encodings_model["data"].push_back(nlohmann::json::array({1.0F}));
		}
		const auto max_encodings_document =
		    decode_models(nlohmann::json::array({max_encodings_model}), true);
		ok &= expect_status(max_encodings_document.result.status, UserModelStatus::kOk,
		                    "max encodings per model returns kOk");
		ok &= expect(max_encodings_document.result.entries.size() == 1,
		             "max encodings per model preserves entry");
		if (!max_encodings_document.result.entries.empty()) {
			ok &= expect(max_encodings_document.result.entries.front().encodings.size() ==
			                 kMaxEncodingsPerModel,
			             "max encodings per model are preserved");
		}

		auto max_models = nlohmann::json::array();
		for (std::size_t index = 0; index < kMaxStoredModels; ++index) {
			max_models.push_back(valid_model_json(static_cast<int>(index)));
		}
		const auto max_models_document = decode_models(max_models, true);
		ok &= expect_status(max_models_document.result.status, UserModelStatus::kOk,
		                    "max stored models returns kOk");
		ok &= expect(max_models_document.result.entries.size() == kMaxStoredModels,
		             "max stored models are preserved");
		ok &= expect(std::cmp_equal(max_models_document.result.next_id, kMaxStoredModels),
		             "max stored models reports next_id");

		return ok;
	}

	auto expect_multi_model_next_id_preserves_entries() -> bool {
		using howdy::native::UserModelStatus;

		bool ok = true;

		auto first     = valid_model_json(10);
		first["label"] = "highest";
		first["time"]  = 1700000010LL;
		first["data"]  = nlohmann::json::array({nlohmann::json::array({10.0F})});

		auto second     = valid_model_json(2);
		second["label"] = "lowest";
		second["time"]  = 1700000002LL;
		second["data"]  = nlohmann::json::array({nlohmann::json::array({2.0F, 2.5F})});

		auto third     = valid_model_json(7);
		third["label"] = "middle";
		third["time"]  = 1700000007LL;
		third["data"]  = nlohmann::json::array({nlohmann::json::array({7.0F})});

		const auto document = decode_models(nlohmann::json::array({first, second, third}), true);
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

		return ok;
	}

	auto expect_collection_limits() -> bool {
		using howdy::native::UserModelStatus;
		using howdy::native::user_model_limits::kMaxEncodingsPerModel;
		using howdy::native::user_model_limits::kMaxStoredModels;

		bool ok = true;

		auto too_many_encodings    = valid_model_json();
		too_many_encodings["data"] = nlohmann::json::array();
		for (std::size_t index = 0; index < kMaxEncodingsPerModel + 1; ++index) {
			too_many_encodings["data"].push_back(nlohmann::json::array({1.0F}));
		}
		ok &= expect_status(
		    decode_models(nlohmann::json::array({too_many_encodings}), true).result.status,
		    UserModelStatus::kOversized, "too many encodings per model returns kOversized");

		auto too_many_models = nlohmann::json::array();
		for (std::size_t index = 0; index < kMaxStoredModels + 1; ++index) {
			too_many_models.push_back(valid_model_json(static_cast<int>(index)));
		}
		ok &=
		    expect_status(decode_models(too_many_models, true).result.status,
		                  UserModelStatus::kOversized, "too many stored models returns kOversized");

		return ok;
	}

}  // namespace

auto main() -> int {
	try {
		bool ok = true;

		ok &= expect_valid_strict_document();
		ok &= expect_encode_decode_round_trip();
		ok &= expect_invalid_documents();
		ok &= expect_malformed_scalar_fields();
		ok &= expect_strict_shape_contract();
		ok &= expect_compatibility_checks();
		ok &= expect_encoding_validation();
		ok &= expect_accepted_boundaries();
		ok &= expect_multi_model_next_id_preserves_entries();
		ok &= expect_collection_limits();

		return ok ? 0 : 1;
	} catch (const std::exception &error) {
		std::cerr << "FAIL: unexpected exception: " << error.what() << "\n";
		return 1;
	} catch (...) {
		std::cerr << "FAIL: unexpected non-standard exception\n";
		return 1;
	}
}
