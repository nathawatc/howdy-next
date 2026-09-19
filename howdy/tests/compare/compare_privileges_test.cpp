#include "compare/compare_privileges_test_groups.hpp"

auto main() -> int {
	bool ok = true;
	ok &= RunComparePrivilegesNonRootTests();
	ok &= RunComparePrivilegesPrivilegedTests();
	ok &= RunComparePrivilegesFatalTests();
	return ok ? 0 : 1;
}
