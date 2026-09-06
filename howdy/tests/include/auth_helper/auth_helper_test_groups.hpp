#pragma once

#include <filesystem>

namespace howdy::native::auth_helper {
	struct AclOperations;
}

namespace howdy::test::auth_helper {
	struct FakeAclContext;
}

struct AuthHelperStagingContext {
	bool                                             acl_functional;
	bool                                             acl_supported;
	uid_t                                            functional_target_uid;
	const howdy::native::auth_helper::AclOperations &operations;
	const howdy::native::auth_helper::AclOperations &production_operations;
	howdy::test::auth_helper::FakeAclContext        &fake_acl;
	bool                                             use_fake_acl;
};

auto run_auth_helper_path_tests(const std::filesystem::path &temp_root) -> bool;
auto run_auth_helper_staging_tests(const std::filesystem::path    &temp_root,
                                   const AuthHelperStagingContext &context) -> bool;
