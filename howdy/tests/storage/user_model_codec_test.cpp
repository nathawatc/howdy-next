#include "storage/user_model_codec_test_support.hpp"

#include <exception>
#include <iostream>

using namespace howdy::test::user_model_codec;

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
