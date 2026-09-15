#include "storage/user_model_codec_test_support.hpp"
#include "storage/user_model_limits.hpp"

#include <utility>

namespace howdy::test::user_model_codec {

	auto ExpectCompatibilityChecks() -> bool {
		using howdy::native::UserModelStatus;

		bool ok = true;
		ok &= ExpectStatus(Decode(ModelList({ModelJson("7", "1700000000", R"("Office camera")",
		                                               R"("other_backend")")}))
		                       .result.status,
		                   UserModelStatus::kIncompatibleBackend,
		                   "backend mismatch returns kIncompatibleBackend");
		ok &= ExpectStatus(Decode(ModelList({ModelJson("7", "1700000000", R"("Office camera")",
		                                               R"("opencv_dnn_sface")", R"("l2")")}))
		                       .result.status,
		                   UserModelStatus::kIncompatibleMetric,
		                   "metric mismatch returns kIncompatibleMetric");
		ok &= ExpectStatus(Decode(ModelList({ModelJson("7", "1700000000", R"("Office camera")",
		                                               R"("opencv_dnn_sface")", R"("cosine")",
		                                               R"("other.onnx")")}))
		                       .result.status,
		                   UserModelStatus::kIncompatibleModel,
		                   "model mismatch returns kIncompatibleModel");
		return ok;
	}

	namespace {

		auto RepeatedArray(std::size_t count, std::string_view value) -> std::string {
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

		auto NestedArray(std::size_t depth) -> std::string {
			return std::string(depth, '[') + "0" + std::string(depth, ']');
		}

	}  // namespace

	auto ExpectBoundariesAndLimits() -> bool {
		using howdy::native::UserModelStatus;
		using howdy::native::user_model_limits::kMaxEncodingLength;
		using howdy::native::user_model_limits::kMaxEncodingsPerModel;
		using howdy::native::user_model_limits::kMaxStoredModels;

		bool       ok                    = true;
		const auto max_encoding          = RepeatedArray(kMaxEncodingLength, "0.0");
		const auto max_encoding_document = Decode(
		    ModelList({ModelJson("7", "1700000000", R"("Office camera")", R"("opencv_dnn_sface")",
		                         R"("cosine")", R"("sface.onnx")", "[" + max_encoding + "]")}));
		ok &= ExpectStatus(max_encoding_document.result.status, UserModelStatus::kOk,
		                   "max encoding length returns kOk");
		ok &= Expect(max_encoding_document.result.entries.front().encodings.front().size() ==
		                 kMaxEncodingLength,
		             "max encoding length is preserved");

		const auto max_encodings          = RepeatedArray(kMaxEncodingsPerModel, "[1.0]");
		const auto max_encodings_document = Decode(
		    ModelList({ModelJson("7", "1700000000", R"("Office camera")", R"("opencv_dnn_sface")",
		                         R"("cosine")", R"("sface.onnx")", max_encodings)}));
		ok &= ExpectStatus(max_encodings_document.result.status, UserModelStatus::kOk,
		                   "max encodings per model returns kOk");
		ok &= Expect(max_encodings_document.result.entries.front().encodings.size() ==
		                 kMaxEncodingsPerModel,
		             "max encodings per model are preserved");

		std::vector<std::string> max_models;
		max_models.reserve(kMaxStoredModels);
		for (std::size_t index = 0; index < kMaxStoredModels; ++index) {
			max_models.push_back(ModelJson(std::to_string(index)));
		}
		const auto max_models_document = Decode(ModelList(max_models));
		ok &= ExpectStatus(max_models_document.result.status, UserModelStatus::kOk,
		                   "max stored models returns kOk");
		ok &= Expect(max_models_document.result.entries.size() == kMaxStoredModels,
		             "max stored models are preserved");
		ok &= Expect(std::cmp_equal(max_models_document.result.next_id, kMaxStoredModels),
		             "max stored models reports next_id");

		ok &= ExpectStatus(
		    Decode(ModelList({ModelJson("7", "1700000000", R"("Office camera")",
		                                R"("opencv_dnn_sface")", R"("cosine")", R"("sface.onnx")",
		                                "[" + RepeatedArray(kMaxEncodingLength + 1, "0.0") + "]")}))
		        .result.status,
		    UserModelStatus::kOversized, "oversized encoding length returns kOversized");
		ok &= ExpectStatus(
		    Decode(ModelList({ModelJson("7", "1700000000", R"("Office camera")",
		                                R"("opencv_dnn_sface")", R"("cosine")", R"("sface.onnx")",
		                                RepeatedArray(kMaxEncodingsPerModel + 1, "[1.0]"))}))
		        .result.status,
		    UserModelStatus::kOversized, "too many encodings per model returns kOversized");
		max_models.push_back(ModelJson(std::to_string(kMaxStoredModels)));
		ok &= ExpectStatus(Decode(ModelList(max_models)).result.status, UserModelStatus::kOversized,
		                   "too many stored models returns kOversized");
		return ok;
	}

	auto ExpectJsonDepthLimit() -> bool {
		using howdy::native::UserModelStatus;
		using howdy::native::user_model_limits::kMaxJsonNestingDepth;

		const auto at_limit = Decode(ModelList({ModelJson(
		    "0", "1", R"("deep")", R"("opencv_dnn_sface")", R"("cosine")", R"("sface.onnx")",
		    "[[0.1]]", ",\"unknown\":" + NestedArray(kMaxJsonNestingDepth - 2))}));
		bool       ok       = ExpectStatus(at_limit.result.status, UserModelStatus::kOk,
		                                   "container nesting exactly at limit returns kOk");

		const auto over_limit = Decode(ModelList({ModelJson(
		    "0", "1", R"("deep")", R"("opencv_dnn_sface")", R"("cosine")", R"("sface.onnx")",
		    "[[0.1]]", ",\"unknown\":" + NestedArray(kMaxJsonNestingDepth - 1))}));
		ok &= ExpectStatus(over_limit.result.status, UserModelStatus::kOversized,
		                   "one container beyond nesting limit returns kOversized");
		ok &= Expect(over_limit.result.error_message ==
		                 "User model JSON nesting exceeds safety limit",
		             "nesting limit uses stable error message");
		return ok;
	}

}  // namespace howdy::test::user_model_codec
