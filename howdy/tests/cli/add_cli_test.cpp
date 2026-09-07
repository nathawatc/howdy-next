#include "cli/add_cli_test_support.hpp"

auto main() -> int {
	using namespace howdy::test::add_cli;

	bool ok = true;
	ok &= RunAddCliSuccessTests();
	ok &= RunAddCliPreflightTests();
	ok &= RunAddCliCaptureTests();
	ok &= RunAddCliArgumentTests();
	ok &= RunAddCliDependencyValidationTests();
	return ok ? 0 : 1;
}
