auto RunComparePrivilegesNonRootTests() -> bool;
auto RunComparePrivilegesPrivilegedTests() -> bool;
auto RunComparePrivilegesFatalTests() -> bool;

auto main() -> int {
	bool ok = true;
	ok &= RunComparePrivilegesNonRootTests();
	ok &= RunComparePrivilegesPrivilegedTests();
	ok &= RunComparePrivilegesFatalTests();
	return ok ? 0 : 1;
}
