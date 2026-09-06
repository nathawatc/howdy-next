#include "compare/privileges.hpp"
#include "compare/privileges_internal.hpp"
#include "internal.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <grp.h>
#include <pwd.h>
#include <string>
#include <unistd.h>
#include <vector>

#include <sys/fsuid.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/types.h>

#include <linux/capability.h>

namespace {

	using howdy::native::compare_privileges_internal::GroupIdOutputs;
	using howdy::native::compare_privileges_internal::UserIdOutputs;

	constexpr std::size_t kInitialPasswdBufferSize = 1024;
	constexpr std::size_t kMaximumPasswdBufferSize = std::size_t{64} * 1024;

	auto system_getpwnam_r([[maybe_unused]] void *context, const char *name, passwd *pwd,
	                       char *buffer, std::size_t buffer_size, passwd **result) -> int {
		return getpwnam_r(name, pwd, buffer, buffer_size, result);
	}

	auto system_prctl([[maybe_unused]] void *context, int operation, unsigned long argument2,
	                  unsigned long argument3, unsigned long argument4, unsigned long argument5)
	    -> int {
		return prctl(operation, argument2, argument3, argument4, argument5);
	}

	auto system_getgroups([[maybe_unused]] void *context, int size, gid_t *groups) -> int {
		return getgroups(size, groups);
	}

	auto system_setgroups([[maybe_unused]] void *context, std::size_t count, const gid_t *groups)
	    -> int {
		return setgroups(count, groups);
	}

	auto system_setresgid([[maybe_unused]] void *context, gid_t real, gid_t effective, gid_t saved)
	    -> int {
		return setresgid(real, effective, saved);
	}

	auto system_setresuid([[maybe_unused]] void *context, uid_t real, uid_t effective, uid_t saved)
	    -> int {
		return setresuid(real, effective, saved);
	}

	auto system_capset([[maybe_unused]] void *context, const __user_cap_header_struct *header,
	                   const __user_cap_data_struct *data) -> int {
		return static_cast<int>(syscall(SYS_capset, header, data));
	}

	auto system_capget([[maybe_unused]] void *context, __user_cap_header_struct *header,
	                   __user_cap_data_struct *data) -> int {
		return static_cast<int>(syscall(SYS_capget, header, data));
	}

	auto system_getresgid([[maybe_unused]] void *context, GroupIdOutputs outputs) -> int {
		return getresgid(outputs.real, outputs.effective, outputs.saved);
	}

	auto system_getresuid([[maybe_unused]] void *context, UserIdOutputs outputs) -> int {
		return getresuid(outputs.real, outputs.effective, outputs.saved);
	}

	auto system_query_fsuid([[maybe_unused]] void *context) -> uid_t {
		return static_cast<uid_t>(setfsuid(static_cast<uid_t>(-1)));
	}

	auto system_query_fsgid([[maybe_unused]] void *context) -> gid_t {
		return static_cast<gid_t>(setfsgid(static_cast<gid_t>(-1)));
	}

	auto system_setuid([[maybe_unused]] void *context, uid_t uid) -> int {
		return setuid(uid);
	}

}  // namespace

namespace howdy::native::compare_privileges_internal {

	auto default_dependencies() -> ComparePrivilegeDependencies {
		return {
		    .context       = nullptr,
		    .getpwnam_r    = system_getpwnam_r,
		    .prctl         = system_prctl,
		    .getgroups     = system_getgroups,
		    .setgroups     = system_setgroups,
		    .setresgid     = system_setresgid,
		    .setresuid     = system_setresuid,
		    .capset        = system_capset,
		    .capget        = system_capget,
		    .getresgid     = system_getresgid,
		    .getresuid     = system_getresuid,
		    .query_fsuid   = system_query_fsuid,
		    .query_fsgid   = system_query_fsgid,
		    .regain_setuid = system_setuid,
		    .fatal_exit    = fatal_compare_privilege_failure,
		};
	}

	auto lookup_nobody(const ComparePrivilegeDependencies &dependencies) -> NobodyLookup {
		passwd            nobody{};
		passwd           *lookup_result = nullptr;
		std::vector<char> buffer(kInitialPasswdBufferSize);
		while (true) {
			lookup_result = nullptr;
			const int lookup_error =
			    dependencies.getpwnam_r(dependencies.context, "nobody", &nobody, buffer.data(),
			                            buffer.size(), &lookup_result);
			if (lookup_error == 0) {
				break;
			}
			if (lookup_error != ERANGE) {
				return {.error = {.status        = ComparePrivilegeStatus::kLookupFailed,
				                  .error_message = "nobody account lookup failed: " +
				                                   std::string(std::strerror(lookup_error))}};
			}
			if (buffer.size() >= kMaximumPasswdBufferSize) {
				return {.error = {.status        = ComparePrivilegeStatus::kLookupFailed,
				                  .error_message = "nobody account lookup exceeded 64 KiB"}};
			}
			buffer.resize(std::min(buffer.size() * 2, kMaximumPasswdBufferSize));
		}
		if (lookup_result == nullptr) {
			return {.error = {.status        = ComparePrivilegeStatus::kLookupFailed,
			                  .error_message = "nobody account not found"}};
		}
		if (lookup_result->pw_uid == 0 || lookup_result->pw_gid == 0) {
			return {.error = {.status        = ComparePrivilegeStatus::kInvalidIdentity,
			                  .error_message = "nobody account resolves to root identity"}};
		}
		return {.uid = lookup_result->pw_uid, .gid = lookup_result->pw_gid};
	}

}  // namespace howdy::native::compare_privileges_internal
