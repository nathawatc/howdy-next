#pragma once

#include "common/compare_privileges.hpp"

#include <cstddef>
#include <pwd.h>
#include <string_view>

#include <sys/types.h>

#include <linux/capability.h>

namespace howdy::native::compare_privileges_internal {

	inline constexpr std::string_view kFatalDiagnostic =
	    "Fatal: compare privilege verification failed\n";

	using GetpwnamRFn = int (*)(void *context, const char *name, passwd *pwd, char *buffer,
	                            std::size_t buffer_size, passwd **result);
	using PrctlFn     = int (*)(void *context, int operation, unsigned long argument2,
	                            unsigned long argument3, unsigned long argument4,
	                            unsigned long argument5);
	using GetgroupsFn = int (*)(void *context, int size, gid_t *groups);
	using SetgroupsFn = int (*)(void *context, std::size_t count, const gid_t *groups);
	using SetresgidFn = int (*)(void *context, gid_t real, gid_t effective, gid_t saved);
	using SetresuidFn = int (*)(void *context, uid_t real, uid_t effective, uid_t saved);
	using CapsetFn    = int (*)(void *context, const __user_cap_header_struct *header,
	                            const __user_cap_data_struct *data);
	using CapgetFn    = int (*)(void *context, __user_cap_header_struct *header,
	                            __user_cap_data_struct *data);

	struct GroupIdOutputs {
		gid_t *real      = nullptr;
		gid_t *effective = nullptr;
		gid_t *saved     = nullptr;
	};

	struct UserIdOutputs {
		uid_t *real      = nullptr;
		uid_t *effective = nullptr;
		uid_t *saved     = nullptr;
	};

	using GetresgidFn  = int (*)(void *context, GroupIdOutputs outputs);
	using GetresuidFn  = int (*)(void *context, UserIdOutputs outputs);
	using QueryFsuidFn = uid_t (*)(void *context);
	using QueryFsgidFn = gid_t (*)(void *context);
	using SetuidFn     = int (*)(void *context, uid_t uid);

	struct FatalExitRequest {
		const char *message      = nullptr;
		std::size_t message_size = 0;
		int         exit_code    = 0;
	};

	using FatalExitFn = void (*)(void *context, FatalExitRequest request);

	struct ComparePrivilegeDependencies {
		void        *context       = nullptr;
		GetpwnamRFn  getpwnam_r    = nullptr;
		PrctlFn      prctl         = nullptr;
		GetgroupsFn  getgroups     = nullptr;
		SetgroupsFn  setgroups     = nullptr;
		SetresgidFn  setresgid     = nullptr;
		SetresuidFn  setresuid     = nullptr;
		CapsetFn     capset        = nullptr;
		CapgetFn     capget        = nullptr;
		GetresgidFn  getresgid     = nullptr;
		GetresuidFn  getresuid     = nullptr;
		QueryFsuidFn query_fsuid   = nullptr;
		QueryFsgidFn query_fsgid   = nullptr;
		SetuidFn     regain_setuid = nullptr;
		FatalExitFn  fatal_exit    = nullptr;
	};

	[[noreturn]] void fatal_compare_privilege_failure(void *context, FatalExitRequest request);

	[[nodiscard]] auto drop_compare_privileges(const ComparePrivilegeDependencies &dependencies)
	    -> ComparePrivilegeResult;

}  // namespace howdy::native::compare_privileges_internal
