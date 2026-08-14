auto run_compare_privileges_non_root_tests() -> bool;
auto run_compare_privileges_privileged_tests() -> bool;
auto run_compare_privileges_fatal_tests() -> bool;

auto main() -> int {
	bool ok = true;
	ok &= run_compare_privileges_non_root_tests();
	ok &= run_compare_privileges_privileged_tests();
	ok &= run_compare_privileges_fatal_tests();
	return ok ? 0 : 1;
}
