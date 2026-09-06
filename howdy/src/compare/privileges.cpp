#include "compare/privileges.hpp"

#include "compare/privileges_internal.hpp"
#include "privileges/internal.hpp"

#include <sys/types.h>

namespace howdy::native::compare_privileges_internal {

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
		return compare_privileges_internal::drop_compare_privileges(
		    compare_privileges_internal::default_dependencies());
	}

}  // namespace howdy::native
