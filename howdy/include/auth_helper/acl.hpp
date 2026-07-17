#pragma once

#include <filesystem>
#include <unistd.h>

#include <sys/acl.h>

namespace howdy::native::auth_helper {
	struct AclOperations {
		void *context                                               = nullptr;
		auto (*acl_get_fd)(void *context, int fd) -> acl_t          = nullptr;
		auto (*acl_set_fd)(void *context, int fd, acl_t acl) -> int = nullptr;
	};

	auto production_acl_operations() -> AclOperations;

	__attribute__((visibility("hidden"))) auto
	set_private_acl_with_operations(int fd, const std::filesystem::path &path, uid_t uid,
	                                bool directory, const AclOperations &operations) -> bool;

	__attribute__((visibility("hidden"))) auto
	set_private_acl(int fd, const std::filesystem::path &path, uid_t uid, bool directory) -> bool;

}  // namespace howdy::native::auth_helper
