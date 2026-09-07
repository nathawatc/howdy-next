#include "compare/compare_engine_test_support.hpp"

auto main() -> int {
	bool ok = true;
	ok &= RunCompareEngineFrameTests();
	ok &= RunCompareEngineInferenceTests();
	return ok ? 0 : 1;
}
