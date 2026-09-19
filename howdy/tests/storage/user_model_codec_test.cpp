#include "storage/user_model_codec_test_support.hpp"

#include <exception>
#include <iostream>

using howdy::test::user_model_codec::ExpectAdditionalShapeEdges;
using howdy::test::user_model_codec::ExpectBoundariesAndLimits;
using howdy::test::user_model_codec::ExpectCompatibilityChecks;
using howdy::test::user_model_codec::ExpectDocumentEdgeOperations;
using howdy::test::user_model_codec::ExpectDuplicateKeyBehavior;
using howdy::test::user_model_codec::ExpectEncodingValidation;
using howdy::test::user_model_codec::ExpectImmutableToMutableConversion;
using howdy::test::user_model_codec::ExpectJsonDepthLimit;
using howdy::test::user_model_codec::ExpectMalformedScalarFields;
using howdy::test::user_model_codec::ExpectMetricParsing;
using howdy::test::user_model_codec::ExpectMultiModelBehavior;
using howdy::test::user_model_codec::ExpectNewEntryRoundTrip;
using howdy::test::user_model_codec::ExpectStrictParserBehavior;
using howdy::test::user_model_codec::ExpectStrictShapeContract;
using howdy::test::user_model_codec::ExpectUnknownFieldPreservation;
using howdy::test::user_model_codec::ExpectValidStrictDocument;

auto main() -> int {
	try {
		bool ok = true;
		ok &= ExpectValidStrictDocument();
		ok &= ExpectNewEntryRoundTrip();
		ok &= ExpectStrictParserBehavior();
		ok &= ExpectMetricParsing();
		ok &= ExpectDuplicateKeyBehavior();
		ok &= ExpectImmutableToMutableConversion();
		ok &= ExpectMalformedScalarFields();
		ok &= ExpectStrictShapeContract();
		ok &= ExpectCompatibilityChecks();
		ok &= ExpectEncodingValidation();
		ok &= ExpectAdditionalShapeEdges();
		ok &= ExpectDocumentEdgeOperations();
		ok &= ExpectBoundariesAndLimits();
		ok &= ExpectMultiModelBehavior();
		ok &= ExpectUnknownFieldPreservation();
		ok &= ExpectJsonDepthLimit();
		return ok ? 0 : 1;
	} catch (const std::exception &error) {
		std::cerr << "FAIL: unexpected exception: " << error.what() << "\n";
		return 1;
	} catch (...) {
		std::cerr << "FAIL: unexpected non-standard exception\n";
		return 1;
	}
}
