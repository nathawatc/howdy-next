#include "auth_helper/auth_helper_acl_fake.hpp"
#include "auth_helper/auth_helper_acl_probe.hpp"
#include "auth_helper/auth_helper_test_groups.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <unistd.h>

using namespace howdy::test::auth_helper;

auto main() -> int {
	namespace fs = std::filesystem;

	bool       ok = true;
	const auto temp_root =
	    fs::current_path() / ("howdy-auth-helper-test-" + std::to_string(getpid()));
	std::error_code ec;
	fs::remove_all(temp_root, ec);
	fs::create_directories(temp_root, ec);
	ok &= expect(!ec, "creates auth-helper temp root");
	const auto acl_support_result = acl_support(temp_root);
	if (acl_support_result == AclSupport::kUnsupported) {
		std::cerr << "SKIP: filesystem does not support usable POSIX named-user ACLs\n";
	} else if (acl_support_result == AclSupport::kError) {
		ok = false;
	}
	const bool  acl_supported  = acl_support_result == AclSupport::kSupported;
	const bool  acl_functional = acl_support_result != AclSupport::kError;
	const bool  use_fake_acl   = acl_support_result == AclSupport::kUnsupported;
	const uid_t functional_target_uid =
	    use_fake_acl && geteuid() == 0 && getuid() == geteuid() ? 61001 : getuid();
	FakeAclContext fake_acl;
	const auto     production_operations = howdy::native::auth_helper::production_acl_operations();
	const auto     operations = use_fake_acl ? fake_acl.operations() : production_operations;

	ok &= run_auth_helper_path_tests(temp_root, acl_functional, acl_supported, operations, fake_acl,
	                                 use_fake_acl);
	ok &=
	    run_auth_helper_staging_tests(temp_root, {
	                                                 .acl_functional        = acl_functional,
	                                                 .acl_supported         = acl_supported,
	                                                 .functional_target_uid = functional_target_uid,
	                                                 .operations            = operations,
	                                                 .production_operations = production_operations,
	                                                 .fake_acl              = fake_acl,
	                                                 .use_fake_acl          = use_fake_acl,
	                                             });

	fs::remove_all(temp_root, ec);
	return ok ? 0 : 1;
}
