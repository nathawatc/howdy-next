#pragma once

#include "auth_helper/acl.hpp"

#include <filesystem>

struct AuthHelperStagingContext {
	bool                                      acl_functional = false;
	bool                                      acl_supported  = false;
	uid_t                                     functional_target_uid{};
	howdy::native::auth_helper::AclOperations operations;
	howdy::native::auth_helper::AclOperations production_operations;
};

auto RunAuthHelperPathTests(const std::filesystem::path &temp_root) -> bool;
auto RunAuthHelperStagingTests(const std::filesystem::path    &temp_root,
                               const AuthHelperStagingContext &context) -> bool;
