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

auto RunAuthHelperPathTests(const std::filesystem::path &temp_root) -> bool;
auto RunAuthHelperStagingTests(const std::filesystem::path    &temp_root,
                               const AuthHelperStagingContext &context) -> bool;
