#pragma once

#include "support/invoking_user.hpp"

#include <sys/types.h>

namespace howdy::native::cli_internal {

	struct PrivilegeDropOperations {
		void *context                                                 = nullptr;
		int (*initgroups)(void *context, const char *user, gid_t gid) = nullptr;
		int (*setgid)(void *context, gid_t gid)                       = nullptr;
		int (*setuid)(void *context, uid_t uid)                       = nullptr;
		uid_t (*getuid)(void *context)                                = nullptr;
		uid_t (*geteuid)(void *context)                               = nullptr;
		gid_t (*getgid)(void *context)                                = nullptr;
		gid_t (*getegid)(void *context)                               = nullptr;
	};

	[[nodiscard]] auto SystemPrivilegeDropOperations() noexcept -> PrivilegeDropOperations;

	// Takes on the user's groups, GID and UID for good. On failure the process may hold a mix of
	// both identities: the caller must not carry on with the work it meant to do as the user.
	[[nodiscard]] auto DropPrivilegesPermanently(const InvokingUser            &user,
	                                             const PrivilegeDropOperations &operations) noexcept
	    -> bool;

}  // namespace howdy::native::cli_internal
