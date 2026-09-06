#include "compare/privileges.hpp"
#include "compare/privileges/internal.hpp"
#include "internal.hpp"
#include "protocol/compare_exit.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <unistd.h>

#include <sys/prctl.h>
#include <sys/types.h>

#include <linux/capability.h>
#include <linux/securebits.h>

namespace {

	using CapabilityData = std::array<__user_cap_data_struct, _LINUX_CAPABILITY_U32S_3>;

	auto capabilities_empty(const CapabilityData &data, bool check_inheritable) -> bool {
		return std::ranges::all_of(data, [check_inheritable](const auto &word) -> bool {
			return word.effective == 0 && word.permitted == 0 &&
			       (!check_inheritable || word.inheritable == 0);
		});
	}

	auto clear_and_verify_capabilities(
	    const howdy::native::compare_privileges_internal::ComparePrivilegeDependencies
	                             &dependencies,
	    __user_cap_header_struct &header) -> bool {
		CapabilityData data{};
		if (dependencies.capset(dependencies.context, &header, data.data()) != 0) {
			return false;
		}
		data = {};
		return dependencies.capget(dependencies.context, &header, data.data()) == 0 &&
		       capabilities_empty(data, true);
	}

}  // namespace

namespace howdy::native::compare_privileges_internal {

	auto fatal_verification_failure(const ComparePrivilegeDependencies &dependencies)
	    -> ComparePrivilegeResult {
		constexpr auto diagnostic = kFatalDiagnostic;
		dependencies.fatal_exit(dependencies.context, {.message      = diagnostic.data(),
		                                               .message_size = diagnostic.size(),
		                                               .exit_code    = static_cast<int>(
		                                                   howdy::native::CompareExit::kAbort)});
		return {
		    .status        = ComparePrivilegeStatus::kVerificationFailure,
		    .error_message = "fatal compare privilege callback returned",
		};
	}

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

	auto read_process_identity(const ComparePrivilegeDependencies &dependencies,
	                           ProcessIdentity                    *identity) -> bool {
		return dependencies.getresuid(dependencies.context, {.real      = &identity->real_uid,
		                                                     .effective = &identity->effective_uid,
		                                                     .saved = &identity->saved_uid}) == 0 &&
		       dependencies.getresgid(dependencies.context, {.real      = &identity->real_gid,
		                                                     .effective = &identity->effective_gid,
		                                                     .saved = &identity->saved_gid}) == 0;
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
		if (dependencies.prctl(dependencies.context, PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0,
		                       0) != 0 ||
		    !clear_and_verify_capabilities(dependencies, header) ||
		    dependencies.regain_setuid(dependencies.context, 0) == 0) {
			return fatal_verification_failure(dependencies);
		}
		return {.status = ComparePrivilegeStatus::kOk, .error_message = {}};
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
		if (dependencies.prctl(dependencies.context, PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0,
		                       0) != 0) {
			return {.status        = ComparePrivilegeStatus::kCapabilityFailure,
			        .error_message = "failed to clear ambient capabilities"};
		}
		return {.status = ComparePrivilegeStatus::kOk};
	}

	auto transition_identity(const ComparePrivilegeDependencies &dependencies, uid_t target_uid,
	                         gid_t target_gid) -> bool {
		return dependencies.setgroups(dependencies.context, 0, nullptr) == 0 &&
		       dependencies.setresgid(dependencies.context, target_gid, target_gid, target_gid) ==
		           0 &&
		       dependencies.setresuid(dependencies.context, target_uid, target_uid, target_uid) ==
		           0;
	}

	auto verify_dropped_identity(const ComparePrivilegeDependencies &dependencies, uid_t target_uid,
	                             gid_t target_gid) -> bool {
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
		                                                  .saved     = &identity.saved_gid}) != 0 ||
		    identity.real_gid != target_gid || identity.effective_gid != target_gid ||
		    identity.saved_gid != target_gid) {
			return false;
		}
		if (dependencies.getresuid(dependencies.context, {.real      = &identity.real_uid,
		                                                  .effective = &identity.effective_uid,
		                                                  .saved     = &identity.saved_uid}) != 0 ||
		    identity.real_uid != target_uid || identity.effective_uid != target_uid ||
		    identity.saved_uid != target_uid) {
			return false;
		}
		return dependencies.query_fsuid(dependencies.context) == target_uid &&
		       dependencies.query_fsgid(dependencies.context) == target_gid &&
		       dependencies.getgroups(dependencies.context, 0, nullptr) == 0 &&
		       dependencies.regain_setuid(dependencies.context, 0) != 0;
	}

}  // namespace howdy::native::compare_privileges_internal
