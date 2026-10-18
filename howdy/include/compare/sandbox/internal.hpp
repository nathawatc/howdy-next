#pragma once

#include "compare/sandbox.hpp"
#include "support/prctl_types.hpp"

#include <vector>

#include <sys/resource.h>

#include <linux/filter.h>
#include <linux/seccomp.h>

#ifndef SECCOMP_SET_MODE_FILTER
#	define SECCOMP_SET_MODE_FILTER 1
#endif
#ifndef SECCOMP_FILTER_FLAG_TSYNC
#	define SECCOMP_FILTER_FLAG_TSYNC 1
#endif
#ifndef SECCOMP_RET_KILL_PROCESS
#	define SECCOMP_RET_KILL_PROCESS SECCOMP_RET_KILL
#endif
#ifndef SECCOMP_RET_ERRNO
#	define SECCOMP_RET_ERRNO 0x00050000U
#endif
#ifndef SECCOMP_RET_DATA
#	define SECCOMP_RET_DATA 0x0000ffffU
#endif

namespace howdy::native::compare_sandbox_internal {

	using RlimitResource = decltype(RLIMIT_CPU);
	using GetrlimitFn    = int (*)(void *context, RlimitResource resource, rlimit *limit);
	using SetrlimitFn    = int (*)(void *context, RlimitResource resource, const rlimit *limit);
	using PrctlFn        = int (*)(void *context, int operation, PrctlArgument argument2,
	                               PrctlArgument argument3, PrctlArgument argument4,
	                               PrctlArgument argument5);
	using SeccompFn      = int (*)(void *context, unsigned int operation, unsigned int flags,
	                               void *args);

	struct CompareSandboxDependencies {
		void       *context   = nullptr;
		GetrlimitFn getrlimit = nullptr;
		SetrlimitFn setrlimit = nullptr;
		PrctlFn     prctl     = nullptr;
	};

	struct CompareSeccompDependencies {
		void     *context = nullptr;
		SeccompFn seccomp = nullptr;
	};

	[[nodiscard]] auto BuildCompareSeccompFilter() -> std::vector<struct sock_filter>;

	[[nodiscard]] auto ApplyCompareSandbox(int                               timeout_seconds,
	                                       const CompareSandboxDependencies &dependencies)
	    -> CompareSandboxResult;

	[[nodiscard]] auto ApplyCompareSeccompFilter(const CompareSeccompDependencies &dependencies)
	    -> CompareSeccompResult;

}  // namespace howdy::native::compare_sandbox_internal
