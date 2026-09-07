#include "compare/privileges.hpp"

#include "compare/privileges/internal.hpp"
#include "privileges/internal.hpp"

#include <sys/types.h>

namespace howdy::native::compare_privileges_internal {

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

		ProcessIdentity identity;
		if (!ReadProcessIdentity(dependencies, &identity)) {
			return FatalVerificationFailure(dependencies);
		}

		if (identity.effective_uid != 0) {
			// Non-root compare preserves caller supplementary groups for PAM compatibility.
			// Filesystem and device access therefore remains limited by caller group membership.
			return HandleUnprivilegedCaller(dependencies, identity.real_uid, identity.effective_uid,
			                                identity.saved_uid, identity.real_gid,
			                                identity.effective_gid, identity.saved_gid);
		}

		const auto nobody = LookupNobody(dependencies);
		if (nobody.error.status != ComparePrivilegeStatus::kOk) {
			return nobody.error;
		}
		const uid_t target_uid = nobody.uid;
		const gid_t target_gid = nobody.gid;

		auto capability_result = PrepareCapabilityDrop(dependencies);
		if (!capability_result.Ok()) {
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

}  // namespace howdy::native::compare_privileges_internal

namespace howdy::native {

	auto DropComparePrivileges() -> ComparePrivilegeResult {
		return compare_privileges_internal::DropComparePrivileges(
		    compare_privileges_internal::DefaultDependencies());
	}

}  // namespace howdy::native
