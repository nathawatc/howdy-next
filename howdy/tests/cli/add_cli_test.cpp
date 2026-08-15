#include "cli/add_cli_test_support.hpp"

auto main() -> int {
	using namespace howdy::test::add_cli;

	bool ok = true;
	ok &= run_add_cli_success_tests();
	ok &= run_add_cli_preflight_tests();
	ok &= run_add_cli_capture_tests();
	ok &= run_add_cli_argument_tests();
	ok &= run_add_cli_dependency_validation_tests();
	return ok ? 0 : 1;
}
