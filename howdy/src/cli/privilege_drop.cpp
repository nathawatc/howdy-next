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
		    .getuid = [](void * /*context*/) -> uid_t {
			    return ::getuid();
		    },
		    .geteuid = [](void * /*context*/) -> uid_t {
			    return ::geteuid();
		    },
		    .getgid = [](void * /*context*/) -> gid_t {
			    return ::getgid();
		    },
		    .getegid = [](void * /*context*/) -> gid_t {
			    return ::getegid();
		    },
		};
	}

	// Reports no more than success: the config editor calls this between fork and exec, where it
	// should not build messages or throw. initgroups may still allocate and read the group
	// database, which is safe there only because `howdy config` forks while single-threaded.
	auto DropPrivilegesPermanently(const InvokingUser            &user,
	                               const PrivilegeDropOperations &operations) noexcept -> bool {
		if (operations.initgroups == nullptr || operations.setgid == nullptr ||
		    operations.setuid == nullptr || operations.getuid == nullptr ||
		    operations.geteuid == nullptr || operations.getgid == nullptr ||
		    operations.getegid == nullptr) {
			return false;
		}

		// setuid must come last: initgroups and setgid need root.
		if (operations.initgroups(operations.context, user.name.c_str(), user.gid) != 0 ||
		    operations.setgid(operations.context, user.gid) != 0 ||
		    operations.setuid(operations.context, user.uid) != 0) {
			return false;
		}
		return operations.getuid(operations.context) == user.uid &&
		       operations.geteuid(operations.context) == user.uid &&
		       operations.getgid(operations.context) == user.gid &&
		       operations.getegid(operations.context) == user.gid;
	}

}  // namespace howdy::native::cli_internal
