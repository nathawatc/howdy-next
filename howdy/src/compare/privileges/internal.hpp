#pragma once

#include "compare/privileges.hpp"
#include "compare/privileges/internal.hpp"

#include <sys/types.h>

namespace howdy::native::compare_privileges_internal {

	struct __attribute__((visibility("hidden"))) ProcessIdentity {
		uid_t real_uid      = 0;
		uid_t effective_uid = 0;
		uid_t saved_uid     = 0;
		gid_t real_gid      = 0;
		gid_t effective_gid = 0;
		gid_t saved_gid     = 0;
	};

	struct __attribute__((visibility("hidden"))) NobodyLookup {
		uid_t                  uid = 0;
		gid_t                  gid = 0;
		ComparePrivilegeResult error{.status = ComparePrivilegeStatus::kOk};
	};

	[[nodiscard]] __attribute__((visibility("hidden"))) auto DefaultDependencies()
	    -> ComparePrivilegeDependencies;

	[[nodiscard]] __attribute__((visibility("hidden"))) auto
	LookupNobody(const ComparePrivilegeDependencies &dependencies) -> NobodyLookup;

	[[nodiscard]] __attribute__((visibility("hidden"))) auto
	FatalVerificationFailure(const ComparePrivilegeDependencies &dependencies)
	    -> ComparePrivilegeResult;

	[[nodiscard]] __attribute__((visibility("hidden"))) auto
	ReadProcessIdentity(const ComparePrivilegeDependencies &dependencies, ProcessIdentity *identity)
	    -> bool;

	[[nodiscard]] __attribute__((visibility("hidden"))) auto
	HandleUnprivilegedCaller(const ComparePrivilegeDependencies &dependencies, uid_t real_uid,
	                         uid_t effective_uid, uid_t saved_uid, gid_t real_gid,
	                         gid_t effective_gid, gid_t saved_gid) -> ComparePrivilegeResult;

	[[nodiscard]] __attribute__((visibility("hidden"))) auto
	PrepareCapabilityDrop(const ComparePrivilegeDependencies &dependencies)
	    -> ComparePrivilegeResult;

	[[nodiscard]] __attribute__((visibility("hidden"))) auto
	TransitionIdentity(const ComparePrivilegeDependencies &dependencies, uid_t target_uid,
	                   gid_t target_gid) -> bool;

	[[nodiscard]] __attribute__((visibility("hidden"))) auto
	VerifyDroppedIdentity(const ComparePrivilegeDependencies &dependencies, uid_t target_uid,
	                      gid_t target_gid) -> bool;

}  // namespace howdy::native::compare_privileges_internal
