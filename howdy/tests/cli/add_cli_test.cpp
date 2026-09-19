#include "cli/add_cli_test_support.hpp"

auto main() -> int {
	using howdy::test::add_cli::RunAddCliArgumentTests;
	using howdy::test::add_cli::RunAddCliCaptureTests;
	using howdy::test::add_cli::RunAddCliDependencyValidationTests;
	using howdy::test::add_cli::RunAddCliPreflightTests;
	using howdy::test::add_cli::RunAddCliSuccessTests;

	bool ok = true;
	ok &= RunAddCliSuccessTests();
	ok &= RunAddCliPreflightTests();
	ok &= RunAddCliCaptureTests();
	ok &= RunAddCliArgumentTests();
	ok &= RunAddCliDependencyValidationTests();
	return ok ? 0 : 1;
}
