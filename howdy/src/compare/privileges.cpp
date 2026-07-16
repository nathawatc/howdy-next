#include "compare/privileges.hpp"

#include "compare/privileges_internal.hpp"
#include "protocol/compare_exit.hpp"

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
		dependencies.fatal_exit(dependencies.context, {.message      = diagnostic.data(),
		                                               .message_size = diagnostic.size(),
		                                               .exit_code    = static_cast<int>(
		                                                   howdy::native::CompareExit::kAbort)});
		return {
		    .status        = howdy::native::ComparePrivilegeStatus::kVerificationFailure,
		    .error_message = "fatal compare privilege callback returned",
		};
	}

}  // namespace

namespace howdy::native::compare_privileges_internal {
	namespace {
		using CapabilityData = std::array<__user_cap_data_struct, _LINUX_CAPABILITY_U32S_3>;

		auto capabilities_empty(const CapabilityData &data, bool check_inheritable) -> bool {
			return std::ranges::all_of(data, [check_inheritable](const auto &word) -> bool {
				return word.effective == 0 && word.permitted == 0 &&
				       (!check_inheritable || word.inheritable == 0);
			});
		}

		auto clear_and_verify_capabilities(const ComparePrivilegeDependencies &dependencies,
		                                   __user_cap_header_struct           &header) -> bool {
			CapabilityData data{};
			if (dependencies.capset(dependencies.context, &header, data.data()) != 0) {
				return false;
			}
			data = {};
			return dependencies.capget(dependencies.context, &header, data.data()) == 0 &&
			       capabilities_empty(data, true);
		}

		auto handle_unprivileged_caller(const ComparePrivilegeDependencies &dependencies,
		                                uid_t real_uid, uid_t effective_uid, uid_t saved_uid,
		                                gid_t real_gid, gid_t effective_gid, gid_t saved_gid)
		    -> ComparePrivilegeResult {
			if (real_uid == 0 || saved_uid == 0 || real_gid == 0 || effective_gid == 0 ||
			    saved_gid == 0 || real_uid != effective_uid || real_uid != saved_uid ||
			    real_gid != effective_gid || real_gid != saved_gid) {
				return fatal_verification_failure(dependencies);
			}
			if (dependencies.query_fsuid(dependencies.context) != effective_uid ||
			    dependencies.query_fsgid(dependencies.context) != effective_gid) {
				return fatal_verification_failure(dependencies);
			}
			__user_cap_header_struct header{.version = _LINUX_CAPABILITY_VERSION_3, .pid = 0};
			CapabilityData           capability_data{};
			if (dependencies.capget(dependencies.context, &header, capability_data.data()) != 0 ||
			    !capabilities_empty(capability_data, false)) {
				return fatal_verification_failure(dependencies);
			}
			if (dependencies.prctl(dependencies.context, PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL,
			                       0, 0, 0) != 0 ||
			    !clear_and_verify_capabilities(dependencies, header) ||
			    dependencies.regain_setuid(dependencies.context, 0) == 0) {
				return fatal_verification_failure(dependencies);
			}
			return {.status = ComparePrivilegeStatus::kOk, .error_message = {}};
		}

		struct NobodyLookup {
			uid_t                  uid = 0;
			gid_t                  gid = 0;
			ComparePrivilegeResult error{.status = ComparePrivilegeStatus::kOk};
		};

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

		struct ProcessIdentity {
			uid_t real_uid      = 0;
			uid_t effective_uid = 0;
			uid_t saved_uid     = 0;
			gid_t real_gid      = 0;
			gid_t effective_gid = 0;
			gid_t saved_gid     = 0;
		};

		auto read_process_identity(const ComparePrivilegeDependencies &dependencies,
		                           ProcessIdentity                    *identity) -> bool {
			return dependencies.getresuid(dependencies.context,
			                              {.real      = &identity->real_uid,
			                               .effective = &identity->effective_uid,
			                               .saved     = &identity->saved_uid}) == 0 &&
			       dependencies.getresgid(dependencies.context,
			                              {.real      = &identity->real_gid,
			                               .effective = &identity->effective_gid,
			                               .saved     = &identity->saved_gid}) == 0;
		}

		auto prepare_capability_drop(const ComparePrivilegeDependencies &dependencies)
		    -> ComparePrivilegeResult {
			const int securebits =
			    dependencies.prctl(dependencies.context, PR_GET_SECUREBITS, 0, 0, 0, 0);
			if (securebits < 0) {
				return {.status        = ComparePrivilegeStatus::kCapabilityFailure,
				        .error_message = "failed to query securebits"};
			}
			if ((securebits & (SECBIT_KEEP_CAPS | SECBIT_NO_SETUID_FIXUP)) != 0) {
				return {.status        = ComparePrivilegeStatus::kCapabilityFailure,
				        .error_message = "unsafe securebits configuration"};
			}
			if (dependencies.prctl(dependencies.context, PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL,
			                       0, 0, 0) != 0) {
				return {.status        = ComparePrivilegeStatus::kCapabilityFailure,
				        .error_message = "failed to clear ambient capabilities"};
			}
			return {.status = ComparePrivilegeStatus::kOk};
		}

		auto transition_identity(const ComparePrivilegeDependencies &dependencies, uid_t target_uid,
		                         gid_t target_gid) -> bool {
			return dependencies.setgroups(dependencies.context, 0, nullptr) == 0 &&
			       dependencies.setresgid(dependencies.context, target_gid, target_gid,
			                              target_gid) == 0 &&
			       dependencies.setresuid(dependencies.context, target_uid, target_uid,
			                              target_uid) == 0;
		}

		auto verify_dropped_identity(const ComparePrivilegeDependencies &dependencies,
		                             uid_t target_uid, gid_t target_gid) -> bool {
			__user_cap_header_struct capability_header{
			    .version = _LINUX_CAPABILITY_VERSION_3,
			    .pid     = 0,
			};
			if (!clear_and_verify_capabilities(dependencies, capability_header)) {
				return false;
			}
			ProcessIdentity identity;
			if (dependencies.getresgid(dependencies.context, {.real      = &identity.real_gid,
			                                                  .effective = &identity.effective_gid,
			                                                  .saved = &identity.saved_gid}) != 0 ||
			    identity.real_gid != target_gid || identity.effective_gid != target_gid ||
			    identity.saved_gid != target_gid) {
				return false;
			}
			if (dependencies.getresuid(dependencies.context, {.real      = &identity.real_uid,
			                                                  .effective = &identity.effective_uid,
			                                                  .saved = &identity.saved_uid}) != 0 ||
			    identity.real_uid != target_uid || identity.effective_uid != target_uid ||
			    identity.saved_uid != target_uid) {
				return false;
			}
			return dependencies.query_fsuid(dependencies.context) == target_uid &&
			       dependencies.query_fsgid(dependencies.context) == target_gid &&
			       dependencies.getgroups(dependencies.context, 0, nullptr) == 0 &&
			       dependencies.regain_setuid(dependencies.context, 0) != 0;
		}
	}  // namespace

	[[noreturn]] void fatal_compare_privilege_failure([[maybe_unused]] void *context,
	                                                  FatalExitRequest       request) {
		while (request.message_size > 0) {
			const ssize_t written = write(STDERR_FILENO, request.message, request.message_size);
			if (written > 0) {
				request.message += written;
				request.message_size -= static_cast<std::size_t>(written);
				continue;
			}
			if (written < 0 && errno == EINTR) {
				continue;
			}
			break;
		}
		_exit(request.exit_code);
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

		ProcessIdentity identity;
		if (!read_process_identity(dependencies, &identity)) {
			return fatal_verification_failure(dependencies);
		}

		if (identity.effective_uid != 0) {
			// Non-root compare preserves caller supplementary groups for PAM compatibility.
			// Filesystem and device access therefore remains limited by caller group membership.
			return handle_unprivileged_caller(
			    dependencies, identity.real_uid, identity.effective_uid, identity.saved_uid,
			    identity.real_gid, identity.effective_gid, identity.saved_gid);
		}

		const auto nobody = lookup_nobody(dependencies);
		if (nobody.error.status != ComparePrivilegeStatus::kOk) {
			return nobody.error;
		}
		const uid_t target_uid = nobody.uid;
		const gid_t target_gid = nobody.gid;

		auto capability_result = prepare_capability_drop(dependencies);
		if (!capability_result.ok()) {
			return capability_result;
		}
		if (!transition_identity(dependencies, target_uid, target_gid) ||
		    !verify_dropped_identity(dependencies, target_uid, target_gid)) {
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
