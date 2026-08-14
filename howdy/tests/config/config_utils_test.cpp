#include "config/config_utils_test_support.hpp"
#include "test_support.hpp"

#include <filesystem>

auto main() -> int {
	namespace fs = std::filesystem;
	using howdy::test::expect;

	bool                                ok = true;
	std::error_code                     ec;
	howdy::test::ConfigUtilsTestContext context{
	    .temp_root = fs::current_path() / "howdy-config-utils-test-work",
	};
	context.config_path = context.temp_root / "config.ini";

	fs::remove_all(context.temp_root, ec);
	fs::create_directories(context.temp_root, ec);
	ok &= expect(!ec, "create temp root");

	ok &= howdy::test::run_config_read_update_tests(context);
	ok &= howdy::test::run_config_atomic_write_tests(context);
	ok &= howdy::test::run_config_atomic_replace_tests(context);
	ok &= howdy::test::run_config_lock_failure_tests(context);
	ok &= howdy::test::run_config_atomic_replace_tail_tests(context);
	ok &= howdy::test::run_config_path_security_tests(context);

	fs::remove_all(context.temp_root, ec);
	return ok ? 0 : 1;
}
