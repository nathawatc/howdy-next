#pragma once

#include "support/invoking_user.hpp"

#include <sys/types.h>

namespace howdy::native::cli_internal {

	// Real, effective and saved IDs, as getresuid and getresgid report them.
	struct UserIds {
		uid_t real      = 0;
		uid_t effective = 0;
		uid_t saved     = 0;
	};

	struct GroupIds {
		gid_t real      = 0;
		gid_t effective = 0;
		gid_t saved     = 0;
	};

	struct PrivilegeDropOperations {
		void *context                                                 = nullptr;
		int (*initgroups)(void *context, const char *user, gid_t gid) = nullptr;
		int (*setgid)(void *context, gid_t gid)                       = nullptr;
		int (*setuid)(void *context, uid_t uid)                       = nullptr;
		int (*get_user_ids)(void *context, UserIds *ids)              = nullptr;
		int (*get_group_ids)(void *context, GroupIds *ids)            = nullptr;
	};

	[[nodiscard]] auto SystemPrivilegeDropOperations() noexcept -> PrivilegeDropOperations;

	// Takes on the user's groups, GID and UID for good. On failure the process may hold a mix of
	// both identities: the caller must not carry on with the work it meant to do as the user.
	[[nodiscard]] auto DropPrivilegesPermanently(const InvokingUser            &user,
	                                             const PrivilegeDropOperations &operations) noexcept
	    -> bool;

}  // namespace howdy::native::cli_internal
