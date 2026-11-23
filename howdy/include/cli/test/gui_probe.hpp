#pragma once

#include "support/invoking_user.hpp"

#include <cstddef>
#include <optional>
#include <string>

#include <sys/types.h>

namespace howdy::native::test_cli_internal {

	struct GuiProbeOperations {
		void *context                                                           = nullptr;
		int (*getgroups)(void *context, int size, gid_t *groups)                = nullptr;
		int (*setgroups)(void *context, std::size_t count, const gid_t *groups) = nullptr;
		int (*initgroups)(void *context, const char *user, gid_t gid)           = nullptr;
		gid_t (*getegid)(void *context)                                         = nullptr;
		uid_t (*geteuid)(void *context)                                         = nullptr;
		int (*setegid)(void *context, gid_t gid)                                = nullptr;
		int (*seteuid)(void *context, uid_t uid)                                = nullptr;
		void (*probe)(void *context, const InvokingUser &user)                  = nullptr;
	};

	// Runs `probe` under the invoking user's effective IDs and groups, then restores root's.
	// Requires effective UID 0. Returns an error message on failure.
	[[nodiscard]] auto RunGuiProbeAsInvokingUser(const InvokingUser       &user,
	                                             const GuiProbeOperations &operations)
	    -> std::optional<std::string>;

}  // namespace howdy::native::test_cli_internal
