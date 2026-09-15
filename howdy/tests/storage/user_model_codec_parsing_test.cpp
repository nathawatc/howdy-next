#include "storage/user_model_codec_test_support.hpp"
#include "storage/user_model_limits.hpp"

#include <limits>

namespace howdy::test::user_model_codec {

	auto ExpectMetricParsing() -> bool {
		using howdy::native::FaceMetric;
		using howdy::native::UserModelStatus;

		bool       ok            = true;
		const auto expect_metric = [&](std::string_view spelling, FaceMetric expected) -> void {
			const auto metric_json = std::string("\"") + std::string(spelling) + "\"";
			const auto document    = howdy::native::user_model_codec::DecodeDocument(
			    ModelList({ModelJson("7", "1700000000", R"("Office camera")",
			                         R"("opencv_dnn_sface")", metric_json, R"("sface.onnx")")}),
			    kBackend, std::nullopt, kModel);
			ok &= ExpectStatus(document.result.status, UserModelStatus::kOk,
			                   std::string(spelling) + " stored metric is accepted");
			ok &= Expect(!document.result.entries.empty() &&
			                 document.result.entries.front().metric.has_value() &&
			                 *document.result.entries.front().metric == expected,
			             std::string(spelling) + " stored metric parses to typed policy");
		};

		expect_metric("cosine", FaceMetric::kCosine);
		expect_metric("l2", FaceMetric::kL2);
		expect_metric("l2norm", FaceMetric::kL2Norm);
		expect_metric("L2NORM", FaceMetric::kL2Norm);

		const auto unknown = howdy::native::user_model_codec::DecodeDocument(
		    ModelList({ModelJson("7", "1700000000", R"("Office camera")", R"("opencv_dnn_sface")",
		                         R"("euclidean")", R"("sface.onnx")")}),
		    kBackend, std::nullopt, kModel);
		ok &= ExpectStatus(unknown.result.status, UserModelStatus::kParseError,
		                   "unknown stored metric is rejected");
		ok &= Expect(unknown.result.error_message.contains("unknown face metric"),
		             "unknown stored metric reports explicit error");

		const auto legacy = howdy::native::user_model_codec::DecodeDocument(
		    ModelList({ModelJson("7", "1700000000", R"("Office camera")", R"("opencv_dnn_sface")",
		                         R"("L2NORM")", R"("sface.onnx")")}),
		    kBackend, std::nullopt, kModel);
		const auto serialized = howdy::native::user_model_codec::SerializeDocument(legacy);
		ok &= Expect(serialized.has_value() && serialized->contains(R"("metric":"L2NORM")"),
		             "existing persisted metric spelling is preserved");

		const auto missing = howdy::native::user_model_codec::DecodeDocument(
		    R"([{"id":7,"time":1700000000,"label":"legacy","backend":"opencv_dnn_sface","model":"sface.onnx","data":[[1.0]]}])",
		    kBackend, FaceMetric::kCosine, kModel);
		ok &= ExpectStatus(missing.result.status, UserModelStatus::kOk,
		                   "missing persisted metric remains compatible");
		ok &= Expect(!missing.result.entries.empty() &&
		                 !missing.result.entries.front().metric.has_value(),
		             "missing persisted metric remains absent");
		return ok;
	}

	auto ExpectStrictParserBehavior() -> bool {
		using howdy::native::UserModelStatus;

		bool ok = true;
		ok &= ExpectStatus(Decode("[").result.status, UserModelStatus::kParseError,
		                   "malformed JSON returns kParseError");
		const auto malformed = Decode("[");
		ok &= Expect(malformed.result.error_message == "Failed to parse user model JSON",
		             "parser failures use stable Howdy message");
		ok &= ExpectStatus(Decode("{}").result.status, UserModelStatus::kInvalidShape,
		                   "non-array root returns kInvalidShape");
		ok &= ExpectStatus(Decode("[]").result.status, UserModelStatus::kNoModel,
		                   "empty array returns kNoModel");

		const std::string bom("\xEF\xBB\xBF", 3);
		ok &= ExpectStatus(Decode(bom + ModelList({ModelJson()})).result.status,
		                   UserModelStatus::kOk, "UTF-8 BOM is accepted");
		ok &= ExpectStatus(Decode("[/*comment*/" + ModelJson() + "]").result.status,
		                   UserModelStatus::kParseError, "comments are rejected");
		ok &= ExpectStatus(Decode("[" + ModelJson() + ",]").result.status,
		                   UserModelStatus::kParseError, "trailing comma is rejected");
		ok &= ExpectStatus(Decode(ModelList({ModelJson()}) + "[]").result.status,
		                   UserModelStatus::kParseError, "second JSON document is rejected");
		ok &= ExpectStatus(Decode(ModelList({ModelJson()}) + "junk").result.status,
		                   UserModelStatus::kParseError, "trailing junk is rejected");

		std::string invalid_utf8   = ModelList({ModelJson()});
		const auto  label_offset   = invalid_utf8.find("Office camera");
		invalid_utf8[label_offset] = static_cast<char>(0xFF);
		ok &= ExpectStatus(Decode(invalid_utf8).result.status, UserModelStatus::kParseError,
		                   "invalid UTF-8 is rejected");
		for (const std::string_view number : {"NaN", "Infinity", "1e999"}) {
			ok &= ExpectStatus(
			    Decode(ModelList({ModelJson("7", "1700000000", R"("Office camera")",
			                                R"("opencv_dnn_sface")", R"("cosine")",
			                                R"("sface.onnx")", "[[" + std::string(number) + "]]")}))
			        .result.status,
			    UserModelStatus::kParseError, std::string(number) + " encoding is rejected");
		}
		return ok;
	}

	auto ExpectDuplicateKeyBehavior() -> bool {
		using howdy::native::UserModelStatus;

		bool ok = true;
		ok &=
		    ExpectStatus(Decode(ModelList({ModelJson("7", "1700000000", R"("Office camera")",
		                                             R"("opencv_dnn_sface")", R"("cosine")",
		                                             R"("sface.onnx")", "[[1.0]]", ",\"id\":8")}))
		                     .result.status,
		                 UserModelStatus::kInvalidShape, "duplicate known direct key is rejected");
		ok &= ExpectStatus(
		    Decode(ModelList({ModelJson("7", "1700000000", R"("Office camera")",
		                                R"("opencv_dnn_sface")", R"("cosine")", R"("sface.onnx")",
		                                "[[1.0]]", R"(,"future":1,"future":2)")}))
		        .result.status,
		    UserModelStatus::kInvalidShape, "duplicate unknown direct key is rejected");
		const auto tolerant_duplicate = Decode(
		    ModelList({ModelJson("7", "1700000000", R"("Office camera")", R"("opencv_dnn_sface")",
		                         R"("cosine")", R"("sface.onnx")", "[[1.0]]", ",\"id\":8")}),
		    false);
		ok &= ExpectStatus(tolerant_duplicate.result.status, UserModelStatus::kOk,
		                   "duplicate direct key is tolerated without strict shape");
		ok &= Expect(tolerant_duplicate.result.entries.size() == 1,
		             "tolerant duplicate key preserves one entry");
		ok &= Expect(tolerant_duplicate.result.entries.size() == 1 &&
		                 tolerant_duplicate.result.entries.front().id == 8,
		             "tolerant duplicate key keeps the last ID");
		const auto tolerant_known_fields = Decode(
		    ModelList({ModelJson("7", "1700000000", R"("Office camera")", R"("opencv_dnn_sface")",
		                         R"("cosine")", R"("sface.onnx")", "[[1.0]]",
		                         R"(,"label":"Backup camera","data":[[4.0,5.0]])")}),
		    false);
		ok &= ExpectStatus(tolerant_known_fields.result.status, UserModelStatus::kOk,
		                   "duplicate label and data are tolerated without strict shape");
		ok &= Expect(tolerant_known_fields.result.entries.size() == 1 &&
		                 tolerant_known_fields.result.entries.front().label == "Backup camera",
		             "tolerant duplicate label keeps the last value");
		ok &= Expect(tolerant_known_fields.result.entries.size() == 1 &&
		                 tolerant_known_fields.result.entries.front().encodings ==
		                     std::vector<std::vector<float>>{{4.0F, 5.0F}},
		             "tolerant duplicate data keeps the last value");
		ok &= ExpectStatus(
		    Decode(ModelList({ModelJson("7", "1700000000", R"("Office camera")",
		                                R"("opencv_dnn_sface")", R"("cosine")", R"("sface.onnx")",
		                                "[[1.0]]", R"(,"future":{"revision":1,"revision":2})")}))
		        .result.status,
		    UserModelStatus::kOk, "nested unknown duplicate key is not recursively rejected");
		return ok;
	}

	auto ExpectMalformedScalarFields() -> bool {
		using howdy::native::UserModelStatus;

		bool ok = true;
		ok &= ExpectStatus(Decode(ModelList({ModelJson(R"("7")")})).result.status,
		                   UserModelStatus::kInvalidShape, "string model id returns kInvalidShape");
		ok &= ExpectStatus(Decode(ModelList({ModelJson("7.0")})).result.status,
		                   UserModelStatus::kInvalidShape, "real model id returns kInvalidShape");
		ok &= ExpectStatus(Decode(ModelList({ModelJson("2147483648")})).result.status,
		                   UserModelStatus::kInvalidShape, "out-of-range model id is rejected");
		ok &=
		    ExpectStatus(Decode(ModelList({ModelJson("7", R"("bad")")})).result.status,
		                 UserModelStatus::kInvalidShape, "string model time returns kInvalidShape");
		ok &= ExpectStatus(Decode(ModelList({ModelJson("7", "1.5")})).result.status,
		                   UserModelStatus::kInvalidShape, "real model time returns kInvalidShape");
		ok &=
		    ExpectStatus(Decode(ModelList({ModelJson("7", "18446744073709551615")})).result.status,
		                 UserModelStatus::kInvalidShape, "out-of-range model time is rejected");
		ok &= ExpectStatus(Decode(ModelList({ModelJson("7", "1700000000", "7")})).result.status,
		                   UserModelStatus::kInvalidShape,
		                   "non-string model label returns kInvalidShape");
		ok &= ExpectStatus(
		    Decode(ModelList({ModelJson("7", "1700000000", R"("bad/name")")})).result.status,
		    UserModelStatus::kParseError, "unsafe model label returns kParseError");
		return ok;
	}

	auto ExpectStrictShapeContract() -> bool {
		using howdy::native::UserModelStatus;

		bool              ok = true;
		const auto *const missing_data =
		    R"([{"id":7,"time":1700000000,"label":"Office camera","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx"}])";
		ok &= ExpectStatus(Decode(missing_data, true).result.status, UserModelStatus::kInvalidShape,
		                   "missing data with strict shape returns kInvalidShape");
		const auto tolerant_missing_data = Decode(missing_data, false);
		ok &= ExpectStatus(tolerant_missing_data.result.status, UserModelStatus::kOk,
		                   "missing data without strict shape returns kOk");
		ok &= Expect(tolerant_missing_data.result.entries.size() == 1 &&
		                 tolerant_missing_data.result.entries.front().encodings.empty(),
		             "missing data without strict shape preserves entry with zero encodings");

		const auto non_array_encoding =
		    ModelList({ModelJson("7", "1700000000", R"("Office camera")", R"("opencv_dnn_sface")",
		                         R"("cosine")", R"("sface.onnx")", R"([[1.0],"bad",[2.0]])")});
		const auto tolerant_non_array = Decode(non_array_encoding, false);
		ok &= ExpectStatus(tolerant_non_array.result.status, UserModelStatus::kOk,
		                   "non-array encoding without strict shape returns kOk");
		ok &= Expect(tolerant_non_array.result.entries.size() == 1 &&
		                 tolerant_non_array.result.entries.front().encodings.size() == 2,
		             "non-array encoding without strict shape is skipped");
		ok &= ExpectStatus(Decode(non_array_encoding, true).result.status,
		                   UserModelStatus::kInvalidShape,
		                   "non-array encoding with strict shape returns kInvalidShape");

		const auto *const legacy_missing_id =
		    R"([{"label":"legacy","backend":"opencv_dnn_sface","data":[[1.0]]}])";
		const auto tolerant_missing_id = Decode(legacy_missing_id, false);
		ok &= ExpectStatus(tolerant_missing_id.result.status, UserModelStatus::kOk,
		                   "missing ID without strict shape returns kOk");
		ok &= Expect(tolerant_missing_id.result.entries.front().id == -1,
		             "missing ID without strict shape preserves legacy sentinel");
		return ok;
	}

	auto ExpectEncodingValidation() -> bool {
		using howdy::native::UserModelStatus;
		using howdy::native::user_model_limits::kMaxEncodingLength;

		bool ok = true;
		ok &= ExpectStatus(howdy::native::user_model_codec::ValidateEncoding({}).status,
		                   UserModelStatus::kOversized, "empty encoding returns kOversized");
		ok &= ExpectStatus(
		    howdy::native::user_model_codec::ValidateEncoding({0.0F, 1.0F, -2.5F}).status,
		    UserModelStatus::kOk, "finite valid encoding returns kOk");
		ok &= ExpectStatus(howdy::native::user_model_codec::ValidateEncoding(
		                       {std::numeric_limits<float>::quiet_NaN()})
		                       .status,
		                   UserModelStatus::kInvalidShape, "NaN encoding returns kInvalidShape");
		ok &=
		    ExpectStatus(howdy::native::user_model_codec::ValidateEncoding(
		                     {std::numeric_limits<float>::infinity()})
		                     .status,
		                 UserModelStatus::kInvalidShape, "infinity encoding returns kInvalidShape");
		ok &= ExpectStatus(howdy::native::user_model_codec::ValidateEncoding(
		                       std::vector<float>(kMaxEncodingLength + 1, 0.0F))
		                       .status,
		                   UserModelStatus::kOversized,
		                   "oversized encoding length returns kOversized");
		return ok;
	}

	auto ExpectAdditionalShapeEdges() -> bool {
		using howdy::native::UserModelStatus;

		bool              ok = true;
		const auto *const missing_id =
		    R"([{"time":1700000000,"label":"Office camera","backend":"opencv_dnn_sface","metric":"cosine","model":"sface.onnx","data":[[1.0]]}])";
		ok &= ExpectStatus(Decode(missing_id).result.status, UserModelStatus::kInvalidShape,
		                   "missing strict model ID returns kInvalidShape");
		ok &=
		    ExpectStatus(Decode(ModelList({ModelJson("-1")})).result.status,
		                 UserModelStatus::kInvalidShape, "negative model ID returns kInvalidShape");

		const auto signed_time = Decode(ModelList({ModelJson("7", "-1")}));
		ok &= ExpectStatus(signed_time.result.status, UserModelStatus::kOk,
		                   "negative model timestamp returns kOk");
		ok &= Expect(!signed_time.result.entries.empty() &&
		                 signed_time.result.entries.front().time == -1,
		             "negative model timestamp is preserved");

		const std::vector<std::string> invalid_metadata = {
		    ModelJson("7", "1700000000", R"("Office camera")", "7"),
		    ModelJson("7", "1700000000", R"("Office camera")", R"("opencv_dnn_sface")", "7"),
		    ModelJson("7", "1700000000", R"("Office camera")", R"("opencv_dnn_sface")",
		              R"("cosine")", "7"),
		};
		for (const auto &content : invalid_metadata) {
			ok &= ExpectStatus(Decode(ModelList({content})).result.status,
			                   UserModelStatus::kInvalidShape,
			                   "non-string model metadata returns kInvalidShape");
		}

		const auto tolerant_label = Decode(ModelList({ModelJson("7", "1700000000", "7")}), false);
		ok &= ExpectStatus(tolerant_label.result.status, UserModelStatus::kOk,
		                   "tolerant non-string label returns kOk");
		ok &= Expect(!tolerant_label.result.entries.empty() &&
		                 tolerant_label.result.entries.front().label.empty(),
		             "tolerant non-string label becomes empty");

		ok &= ExpectStatus(Decode(ModelList({ModelJson("7", "1700000000", R"("Office camera")",
		                                               R"("opencv_dnn_sface")", R"("cosine")",
		                                               R"("sface.onnx")", R"("not-an-array")")}))
		                       .result.status,
		                   UserModelStatus::kInvalidShape,
		                   "non-array model data returns kInvalidShape");
		ok &= ExpectStatus(Decode("[7]").result.status, UserModelStatus::kInvalidShape,
		                   "non-object model entry returns kInvalidShape");
		for (const std::string_view value : {"null", "\"not-a-number\"", "{}"}) {
			ok &= ExpectStatus(
			    Decode(ModelList({ModelJson("7", "1700000000", R"("Office camera")",
			                                R"("opencv_dnn_sface")", R"("cosine")",
			                                R"("sface.onnx")", "[[" + std::string(value) + "]]")}))
			        .result.status,
			    UserModelStatus::kInvalidShape,
			    std::string(value) + " encoding element returns kInvalidShape");
		}
		ok &= ExpectStatus(Decode(ModelList({ModelJson("7", "1700000000", R"("Office camera")",
		                                               R"("opencv_dnn_sface")", R"("cosine")",
		                                               R"("sface.onnx")", "[[]]")}))
		                       .result.status,
		                   UserModelStatus::kOversized, "empty stored encoding returns kOversized");

		for (const std::string_view number : {"1e39", "-1e39"}) {
			const auto encoding = "[[" + std::string(number) + "]]";
			const auto document = Decode(ModelList(
			    {ModelJson("7", "1700000000", R"("Office camera")", R"("opencv_dnn_sface")",
			               R"("cosine")", R"("sface.onnx")", encoding)}));
			ok &= ExpectStatus(document.result.status, UserModelStatus::kInvalidShape,
			                   std::string(number) +
			                       " encoding outside float range returns kInvalidShape");
		}

		const auto tolerant_max_id = Decode(ModelList({ModelJson("2147483647")}), false);
		ok &= ExpectStatus(tolerant_max_id.result.status, UserModelStatus::kOk,
		                   "tolerant INT_MAX model ID returns kOk");
		ok &= Expect(tolerant_max_id.result.next_id == std::numeric_limits<int>::max(),
		             "tolerant INT_MAX model ID preserves allocation boundary");
		return ok;
	}

}  // namespace howdy::test::user_model_codec
