#include "compare/privileges.hpp"

#include "compare/privileges/internal.hpp"
#include "privileges/internal.hpp"
#include "support/error_message.hpp"

#include <cerrno>

#include <sys/types.h>

#include <linux/prctl.h>

namespace howdy::native::compare_privileges_internal {
	namespace {

		// commit_creds() resets dumpable to fs.suid_dumpable whenever the effective or filesystem
		// IDs change, so the early DisableCompareTracing() does not survive the drop to nobody.
		// With suid_dumpable=1 a nobody-owned process could then ptrace compare; the entrypoint
		// rejects one that attached in that window. Seccomp blocks prctl later, so this must run
		// before the filter is installed.
		auto KeepNonDumpable(const ComparePrivilegeDependencies &dependencies)
		    -> ComparePrivilegeResult {
			if (dependencies.prctl(dependencies.context, PR_SET_DUMPABLE, 0, 0, 0, 0) != 0) {
				return {
				    .status        = ComparePrivilegeStatus::kVerificationFailure,
				    .error_message = "failed to clear dumpable after credential change: " +
					                 howdy::native::SystemErrorMessage(errno),
				};
			}
			const int dumpable =
			    dependencies.prctl(dependencies.context, PR_GET_DUMPABLE, 0, 0, 0, 0);
			if (dumpable < 0) {
				return {
				    .status        = ComparePrivilegeStatus::kVerificationFailure,
				    .error_message = "failed to verify dumpable after credential change: " +
					                 howdy::native::SystemErrorMessage(errno),
				};
			}
			if (dumpable != 0) {
				return {
				    .status        = ComparePrivilegeStatus::kVerificationFailure,
				    .error_message = "compare is still dumpable after credential change",
				};
			}
			return {.status = ComparePrivilegeStatus::kOk, .error_message = {}};
		}

		auto DropCredentials(const ComparePrivilegeDependencies &dependencies)
		    -> ComparePrivilegeResult {
			const auto identity = ReadProcessIdentity(dependencies);
			if (!identity.has_value()) {
				return FatalVerificationFailure(dependencies);
			}

			if (identity->effective_uid != 0) {
				// Non-root compare preserves caller supplementary groups for PAM compatibility.
				// Filesystem and device access therefore remains limited by caller group
				// membership.
				return HandleUnprivilegedCaller(dependencies, *identity);
			}

			const auto nobody = LookupNobody(dependencies);
			if (!nobody.has_value()) {
				return nobody.error();
			}
			const uid_t target_uid = nobody->uid;
			const gid_t target_gid = nobody->gid;

			auto capability_result = PrepareCapabilityDrop(dependencies);
			if (!ComparePrivilegeSucceeded(capability_result)) {
				return capability_result;
			}
			if (!TransitionIdentity(dependencies, target_uid, target_gid) ||
			    !VerifyDroppedIdentity(dependencies, target_uid, target_gid)) {
				return FatalVerificationFailure(dependencies);
			}

			return {
			    .status        = ComparePrivilegeStatus::kOk,
			    .error_message = {},
			};
		}

	}  // namespace

	auto DropComparePrivileges(const ComparePrivilegeDependencies &dependencies)
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

		auto result = DropCredentials(dependencies);
		if (!ComparePrivilegeSucceeded(result)) {
			return result;
		}
		return KeepNonDumpable(dependencies);
	}

}  // namespace howdy::native::compare_privileges_internal

namespace howdy::native {

	auto DropComparePrivileges() -> ComparePrivilegeResult {
		return compare_privileges_internal::DropComparePrivileges(
		    compare_privileges_internal::DefaultDependencies());
	}

}  // namespace howdy::native
