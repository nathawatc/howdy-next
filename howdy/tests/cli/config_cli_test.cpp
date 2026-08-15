#include "cli/config_cli_test_support.hpp"

#include <unistd.h>

extern "C" auto real_initgroups(const char *user, gid_t group) -> int asm("__real_initgroups");
extern "C" auto wrap_initgroups(const char *user, gid_t group) -> int asm("__wrap_initgroups");

extern "C" auto wrap_initgroups(const char *user, gid_t group) -> int {
	// Unprivileged tests cannot call initgroups even when target identity is unchanged.
	if (group == getgid()) {
		return 0;
	}
	return real_initgroups(user, group);
}

auto main() -> int {
	using namespace howdy::test::config_cli;

	bool ok = true;
	ok &= run_config_cli_callback_exception_test();
	ok &= run_config_cli_integration_tests();
	ok &= run_config_cli_workflow_tests();
	return ok ? 0 : 1;
}
