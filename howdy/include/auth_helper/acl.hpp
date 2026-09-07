#pragma once

#include "storage/staged_runtime_policy.hpp"

#include <filesystem>
#include <unistd.h>

#include <sys/acl.h>

namespace howdy::native::auth_helper {
	struct AclOperations {
		void *context                                               = nullptr;
		auto (*acl_get_fd)(void *context, int fd) -> acl_t          = nullptr;
		auto (*acl_set_fd)(void *context, int fd, acl_t acl) -> int = nullptr;
	};

	auto ProductionAclOperations() -> AclOperations;

	__attribute__((visibility("hidden"))) auto
	SetPersistentAclWithOperations(int fd, const std::filesystem::path &path, uid_t uid,
	                               const StagedAclPolicy &policy, const AclOperations &operations)
	    -> bool;
	__attribute__((visibility("hidden"))) auto
	VerifyPersistentAclWithOperations(int fd, const std::filesystem::path &path, uid_t uid,
	                                  const StagedAclPolicy &policy,
	                                  const AclOperations   &operations) -> bool;

}  // namespace howdy::native::auth_helper
