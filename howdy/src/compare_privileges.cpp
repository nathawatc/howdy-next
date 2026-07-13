#include "common/compare_privileges.hpp"

#include "common/compare_exit.hpp"
#include "common/compare_privileges_internal.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
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
#include <linux/securebits.h>

namespace {

	constexpr std::size_t kInitialPasswdBufferSize = 1024;
	constexpr std::size_t kMaximumPasswdBufferSize = 64 * 1024;

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

	auto system_getresgid([[maybe_unused]] void *context, gid_t *real, gid_t *effective,
	                      gid_t *saved) -> int {
		return getresgid(real, effective, saved);
	}

	auto system_getresuid([[maybe_unused]] void *context, uid_t *real, uid_t *effective,
	                      uid_t *saved) -> int {
		return getresuid(real, effective, saved);
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

	auto default_dependencies()
	    -> howdy::native::compare_privileges_internal::ComparePrivilegeDependencies {
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
		    .fatal_exit =
		        howdy::native::compare_privileges_internal::fatal_compare_privilege_failure,
		};
	}

	auto fatal_verification_failure(
	    const howdy::native::compare_privileges_internal::ComparePrivilegeDependencies
	        &dependencies) -> howdy::native::ComparePrivilegeResult {
		constexpr auto diagnostic = howdy::native::compare_privileges_internal::kFatalDiagnostic;
		dependencies.fatal_exit(dependencies.context, diagnostic.data(), diagnostic.size(),
		                        static_cast<int>(howdy::native::CompareExit::kAbort));
		return {
		    .status        = howdy::native::ComparePrivilegeStatus::kVerificationFailure,
		    .error_message = "fatal compare privilege callback returned",
		};
	}

}  // namespace

namespace howdy::native::compare_privileges_internal {

	[[noreturn]] void fatal_compare_privilege_failure([[maybe_unused]] void *context,
	                                                  const char *message, std::size_t message_size,
	                                                  int exit_code) {
		while (message_size > 0) {
			const ssize_t written = write(STDERR_FILENO, message, message_size);
			if (written > 0) {
				message += written;
				message_size -= static_cast<std::size_t>(written);
				continue;
			}
			if (written < 0 && errno == EINTR) {
				continue;
			}
			break;
		}
		_exit(exit_code);
	}

	auto drop_compare_privileges(const ComparePrivilegeDependencies &dependencies)
	    -> ComparePrivilegeResult {
		if (dependencies.getpwnam_r == nullptr || dependencies.prctl == nullptr ||
		    dependencies.getgroups == nullptr || dependencies.setgroups == nullptr ||
		    dependencies.setresgid == nullptr || dependencies.setresuid == nullptr ||
		    dependencies.capset == nullptr || dependencies.capget == nullptr ||
		    dependencies.getresgid == nullptr || dependencies.getresuid == nullptr ||
		    dependencies.query_fsuid == nullptr || dependencies.query_fsgid == nullptr ||
		    dependencies.regain_setuid == nullptr || dependencies.fatal_exit == nullptr) {
			return {
			    .status        = ComparePrivilegeStatus::kVerificationFailure,
			    .error_message = "credential operations unavailable",
			};
		}

		uid_t real_uid      = 0;
		uid_t effective_uid = 0;
		uid_t saved_uid     = 0;
		if (dependencies.getresuid(dependencies.context, &real_uid, &effective_uid, &saved_uid) !=
		    0) {
			return fatal_verification_failure(dependencies);
		}

		gid_t real_gid      = 0;
		gid_t effective_gid = 0;
		gid_t saved_gid     = 0;
		if (dependencies.getresgid(dependencies.context, &real_gid, &effective_gid, &saved_gid) !=
		    0) {
			return fatal_verification_failure(dependencies);
		}

		if (effective_uid != 0) {
			if (real_uid == 0 || saved_uid == 0 || real_gid == 0 || effective_gid == 0 ||
			    saved_gid == 0 || real_uid != effective_uid || real_uid != saved_uid ||
			    real_gid != effective_gid || real_gid != saved_gid) {
				return fatal_verification_failure(dependencies);
			}
			// Non-root compare preserves caller supplementary groups for PAM compatibility.
			// Filesystem and device access therefore remains limited by caller group membership.
			if (dependencies.query_fsuid(dependencies.context) != effective_uid ||
			    dependencies.query_fsgid(dependencies.context) != effective_gid) {
				return fatal_verification_failure(dependencies);
			}
			__user_cap_header_struct capability_header{
			    .version = _LINUX_CAPABILITY_VERSION_3,
			    .pid     = 0,
			};
			std::array<__user_cap_data_struct, _LINUX_CAPABILITY_U32S_3> capability_data{};
			if (dependencies.capget(dependencies.context, &capability_header,
			                        capability_data.data()) != 0) {
				return fatal_verification_failure(dependencies);
			}
			for (const auto &word : capability_data) {
				if (word.effective != 0 || word.permitted != 0) {
					return fatal_verification_failure(dependencies);
				}
			}

			if (dependencies.prctl(dependencies.context, PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL,
			                       0, 0, 0) != 0) {
				return fatal_verification_failure(dependencies);
			}

			capability_data = {};
			if (dependencies.capset(dependencies.context, &capability_header,
			                        capability_data.data()) != 0) {
				return fatal_verification_failure(dependencies);
			}
			capability_data = {};
			if (dependencies.capget(dependencies.context, &capability_header,
			                        capability_data.data()) != 0) {
				return fatal_verification_failure(dependencies);
			}
			for (const auto &word : capability_data) {
				if (word.effective != 0 || word.permitted != 0 || word.inheritable != 0) {
					return fatal_verification_failure(dependencies);
				}
			}
			if (dependencies.regain_setuid(dependencies.context, 0) == 0) {
				return fatal_verification_failure(dependencies);
			}
			return {
			    .status        = ComparePrivilegeStatus::kOk,
			    .error_message = {},
			};
		}

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
				return {
				    .status = ComparePrivilegeStatus::kLookupFailed,
				    .error_message =
				        "nobody account lookup failed: " + std::string(std::strerror(lookup_error)),
				};
			}
			if (buffer.size() >= kMaximumPasswdBufferSize) {
				return {
				    .status        = ComparePrivilegeStatus::kLookupFailed,
				    .error_message = "nobody account lookup exceeded 64 KiB",
				};
			}
			buffer.resize(std::min(buffer.size() * 2, kMaximumPasswdBufferSize));
		}

		if (lookup_result == nullptr) {
			return {
			    .status        = ComparePrivilegeStatus::kLookupFailed,
			    .error_message = "nobody account not found",
			};
		}
		const uid_t target_uid = lookup_result->pw_uid;
		const gid_t target_gid = lookup_result->pw_gid;
		if (target_uid == 0 || target_gid == 0) {
			return {
			    .status        = ComparePrivilegeStatus::kInvalidIdentity,
			    .error_message = "nobody account resolves to root identity",
			};
		}

		const int securebits =
		    dependencies.prctl(dependencies.context, PR_GET_SECUREBITS, 0, 0, 0, 0);
		if (securebits < 0) {
			return {
			    .status        = ComparePrivilegeStatus::kCapabilityFailure,
			    .error_message = "failed to query securebits",
			};
		}
		if ((securebits & (SECBIT_KEEP_CAPS | SECBIT_NO_SETUID_FIXUP)) != 0) {
			return {
			    .status        = ComparePrivilegeStatus::kCapabilityFailure,
			    .error_message = "unsafe securebits configuration",
			};
		}
		if (dependencies.prctl(dependencies.context, PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0,
		                       0) != 0) {
			return {
			    .status        = ComparePrivilegeStatus::kCapabilityFailure,
			    .error_message = "failed to clear ambient capabilities",
			};
		}
		if (dependencies.setgroups(dependencies.context, 0, nullptr) != 0) {
			return fatal_verification_failure(dependencies);
		}
		if (dependencies.setresgid(dependencies.context, target_gid, target_gid, target_gid) != 0) {
			return fatal_verification_failure(dependencies);
		}
		if (dependencies.setresuid(dependencies.context, target_uid, target_uid, target_uid) != 0) {
			return fatal_verification_failure(dependencies);
		}

		__user_cap_header_struct capability_header{
		    .version = _LINUX_CAPABILITY_VERSION_3,
		    .pid     = 0,
		};
		std::array<__user_cap_data_struct, _LINUX_CAPABILITY_U32S_3> capability_data{};
		if (dependencies.capset(dependencies.context, &capability_header, capability_data.data()) !=
		    0) {
			return fatal_verification_failure(dependencies);
		}
		capability_data = {};
		if (dependencies.capget(dependencies.context, &capability_header, capability_data.data()) !=
		    0) {
			return fatal_verification_failure(dependencies);
		}
		for (const auto &word : capability_data) {
			if (word.effective != 0 || word.permitted != 0 || word.inheritable != 0) {
				return fatal_verification_failure(dependencies);
			}
		}

		real_gid = effective_gid = saved_gid = 0;
		if (dependencies.getresgid(dependencies.context, &real_gid, &effective_gid, &saved_gid) !=
		    0) {
			return fatal_verification_failure(dependencies);
		}

		if (real_gid != target_gid || effective_gid != target_gid || saved_gid != target_gid) {
			return fatal_verification_failure(dependencies);
		}

		real_uid = effective_uid = saved_uid = 0;
		if (dependencies.getresuid(dependencies.context, &real_uid, &effective_uid, &saved_uid) !=
		    0) {
			return fatal_verification_failure(dependencies);
		}
		if (real_uid != target_uid || effective_uid != target_uid || saved_uid != target_uid) {
			return fatal_verification_failure(dependencies);
		}
		if (dependencies.query_fsuid(dependencies.context) != target_uid ||
		    dependencies.query_fsgid(dependencies.context) != target_gid ||
		    dependencies.getgroups(dependencies.context, 0, nullptr) != 0) {
			return fatal_verification_failure(dependencies);
		}
		if (dependencies.regain_setuid(dependencies.context, 0) == 0) {
			return fatal_verification_failure(dependencies);
		}

		return {
		    .status        = ComparePrivilegeStatus::kOk,
		    .error_message = {},
		};
	}

}  // namespace howdy::native::compare_privileges_internal

namespace howdy::native {

	auto drop_compare_privileges() -> ComparePrivilegeResult {
		return compare_privileges_internal::drop_compare_privileges(default_dependencies());
	}

}  // namespace howdy::native
