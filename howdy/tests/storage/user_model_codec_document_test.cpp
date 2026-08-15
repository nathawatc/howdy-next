#include "storage/user_model_codec_test_support.hpp"

namespace howdy::test::user_model_codec {

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

	auto expect_document_edge_operations() -> bool {
		using howdy::native::UserModelEntry;
		using howdy::native::UserModelStatus;
		using howdy::native::user_model_codec::Document;

		bool     ok = true;
		Document empty;
		ok &= expect(howdy::native::user_model_codec::is_empty(empty), "default document is empty");
		ok &= expect(!howdy::native::user_model_codec::serialize_document(empty).has_value(),
		             "default document has no serialization");
		ok &= expect(!howdy::native::user_model_codec::erase_entry(empty, 0),
		             "default document rejects erase");

		auto decoded_empty = decode("[]");
		ok &= expect(howdy::native::user_model_codec::is_empty(decoded_empty),
		             "decoded empty document is empty");
		const auto empty_serialized =
		    howdy::native::user_model_codec::serialize_document(decoded_empty);
		ok &= expect(empty_serialized.has_value() && *empty_serialized == "[]",
		             "immutable empty document serializes");
		ok &= expect(!howdy::native::user_model_codec::erase_entry(decoded_empty, 0),
		             "empty decoded document rejects out-of-range erase");

		Document             document;
		const UserModelEntry entry{
		    .id        = 20,
		    .time      = 21,
		    .label     = "empty-data",
		    .encodings = {},
		};
		ok &= expect(howdy::native::user_model_codec::append_entry(document, entry),
		             "entry with empty optional fields appends");
		const auto serialized = howdy::native::user_model_codec::serialize_document(document);
		ok &= expect(serialized.has_value() && serialized->contains(R"("data":[])"),
		             "entry with empty encodings serializes data array");
		ok &= expect(serialized.has_value() && !serialized->contains("backend") &&
		                 !serialized->contains("metric") && !serialized->contains("model"),
		             "empty optional metadata is omitted");
		if (serialized.has_value()) {
			const auto round_trip = decode(*serialized);
			ok &= expect_status(round_trip.result.status, UserModelStatus::kOk,
			                    "empty encoding round trip returns kOk");
			ok &= expect(round_trip.result.entries.size() == 1 &&
			                 round_trip.result.entries.front().encodings.empty(),
			             "empty encoding round trip preserves empty data");
		}
		ok &= expect(!howdy::native::user_model_codec::erase_entry(document, 1),
		             "out-of-range erase is rejected");
		ok &= expect(howdy::native::user_model_codec::erase_entry(document, 0),
		             "last entry erase succeeds");
		ok &= expect(howdy::native::user_model_codec::is_empty(document),
		             "document is empty after last erase");
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

}  // namespace howdy::test::user_model_codec
