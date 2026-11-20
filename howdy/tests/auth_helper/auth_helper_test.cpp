#include "auth_helper/auth_helper_test_groups.hpp"
#include "test_support.hpp"

#include <filesystem>
#include <string>
#include <system_error>

#include <unistd.h>

using howdy::test::Expect;

auto main() -> int {
	namespace fs = std::filesystem;

	bool       ok = true;
	const auto temp_root =
	    fs::current_path() / ("howdy-auth-helper-test-" + std::to_string(getpid()));
	std::error_code ec;
	fs::remove_all(temp_root, ec);
	fs::create_directories(temp_root, ec);
	ok &= Expect(!ec, "creates auth-helper temp root");

	ok &= RunAuthHelperDiagnosticsTests();
	ok &= RunAuthHelperDescriptorTests(temp_root);

	fs::remove_all(temp_root, ec);
	return ok ? 0 : 1;
}
