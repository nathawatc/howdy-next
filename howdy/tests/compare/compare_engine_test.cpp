#include "compare/compare_engine_test_support.hpp"

auto main() -> int {
	bool ok = true;
	ok &= run_compare_engine_frame_tests();
	ok &= run_compare_engine_inference_tests();
	return ok ? 0 : 1;
}
