#include "cli/privilege_drop.hpp"

#include "support/invoking_user.hpp"

#include <grp.h>
#include <unistd.h>

#include <sys/types.h>

namespace howdy::native::cli_internal {

	auto SystemPrivilegeDropOperations() noexcept -> PrivilegeDropOperations {
		return {
		    .context    = nullptr,
		    .initgroups = [](void * /*context*/, const char *user, gid_t gid) -> int {
			    return ::initgroups(user, gid);
		    },
		    .setgid = [](void * /*context*/, gid_t gid) -> int {
			    return ::setgid(gid);
		    },
		    .setuid = [](void * /*context*/, uid_t uid) -> int {
			    return ::setuid(uid);
		    },
		    .get_user_ids = [](void * /*context*/, UserIds *ids) -> int {
			    return ::getresuid(&ids->real, &ids->effective, &ids->saved);
		    },
		    .get_group_ids = [](void * /*context*/, GroupIds *ids) -> int {
			    return ::getresgid(&ids->real, &ids->effective, &ids->saved);
		    },
		};
	}

	// Reports no more than success: the config editor calls this between fork and exec, where it
	// should not build messages or throw. initgroups may still allocate and read the group
	// database, which is safe there only because `howdy config` forks while single-threaded.
	auto DropPrivilegesPermanently(const InvokingUser            &user,
	                               const PrivilegeDropOperations &operations) noexcept -> bool {
		if (operations.initgroups == nullptr || operations.setgid == nullptr ||
		    operations.setuid == nullptr || operations.get_user_ids == nullptr ||
		    operations.get_group_ids == nullptr) {
			return false;
		}

		// setuid must come last: initgroups and setgid need root.
		if (operations.initgroups(operations.context, user.name.c_str(), user.gid) != 0 ||
		    operations.setgid(operations.context, user.gid) != 0 ||
		    operations.setuid(operations.context, user.uid) != 0) {
			return false;
		}
		// The saved IDs too: a process that could set only its effective UID would keep a saved
		// UID 0, and with it a way back to root.
		UserIds  user_ids;
		GroupIds group_ids;
		return operations.get_user_ids(operations.context, &user_ids) == 0 &&
		       operations.get_group_ids(operations.context, &group_ids) == 0 &&
		       user_ids.real == user.uid && user_ids.effective == user.uid &&
		       user_ids.saved == user.uid && group_ids.real == user.gid &&
		       group_ids.effective == user.gid && group_ids.saved == user.gid;
	}

}  // namespace howdy::native::cli_internal
