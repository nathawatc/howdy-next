#pragma once

#include "compare/sandbox.hpp"
#include "support/prctl_types.hpp"

#include <string_view>
#include <vector>

#include <sys/resource.h>
#include <sys/types.h>

#include <linux/filter.h>
#include <linux/seccomp.h>
#include <linux/types.h>

namespace howdy::native::compare_sandbox_internal {

#ifdef SECCOMP_SET_MODE_FILTER
	inline constexpr __u32 kSeccompSetModeFilter = SECCOMP_SET_MODE_FILTER;
#else
	inline constexpr __u32 kSeccompSetModeFilter = 1U;
#endif
#ifdef SECCOMP_FILTER_FLAG_TSYNC
	inline constexpr __u32 kSeccompFilterFlagTsync = SECCOMP_FILTER_FLAG_TSYNC;
#else
	inline constexpr __u32 kSeccompFilterFlagTsync = 1U;
#endif
#ifdef SECCOMP_RET_KILL_PROCESS
	inline constexpr __u32 kSeccompRetKillProcess = SECCOMP_RET_KILL_PROCESS;
#else
	inline constexpr __u32 kSeccompRetKillProcess = 0x80000000U;
#endif
#ifdef SECCOMP_RET_ERRNO
	inline constexpr __u32 kSeccompRetErrno = SECCOMP_RET_ERRNO;
#else
	inline constexpr __u32 kSeccompRetErrno = 0x00050000U;
#endif
#ifdef SECCOMP_RET_DATA
	inline constexpr __u32 kSeccompRetData = SECCOMP_RET_DATA;
#else
	inline constexpr __u32 kSeccompRetData = 0x0000ffffU;
#endif

	using RlimitResource = decltype(RLIMIT_CPU);
	using GetrlimitFn    = int (*)(void *context, RlimitResource resource, rlimit *limit);
	using SetrlimitFn    = int (*)(void *context, RlimitResource resource, const rlimit *limit);
	using PrctlFn        = howdy::native::PrctlFn;
	using SeccompFn      = int (*)(void *context, unsigned int operation, unsigned int flags,
	                               void *args);
	// Stores the first nonzero TracerPid across /proc/self/task/*/status, or 0 when no thread is
	// traced; returns -1 with errno set on failure.
	using FindTracerPidFn = int (*)(void *context, pid_t *tracer_pid);

	struct CompareSandboxDependencies {
		void       *context   = nullptr;
		GetrlimitFn getrlimit = nullptr;
		SetrlimitFn setrlimit = nullptr;
		PrctlFn     prctl     = nullptr;
	};

	struct CompareTracingDependencies {
		void           *context         = nullptr;
		PrctlFn         prctl           = nullptr;
		FindTracerPidFn find_tracer_pid = nullptr;
	};

	struct CompareSeccompDependencies {
		void     *context = nullptr;
		SeccompFn seccomp = nullptr;
	};

	[[nodiscard]] auto BuildCompareSeccompFilter() -> std::vector<struct sock_filter>;

	[[nodiscard]] auto DisableCompareTracing(const CompareTracingDependencies &dependencies)
	    -> CompareSandboxResult;

	[[nodiscard]] auto ParseTracerPid(std::string_view status, pid_t *tracer_pid) -> bool;

	[[nodiscard]] auto ApplyCompareSandbox(int                               timeout_seconds,
	                                       const CompareSandboxDependencies &dependencies)
	    -> CompareSandboxResult;

	[[nodiscard]] auto ApplyCompareSeccompFilter(const CompareSeccompDependencies &dependencies)
	    -> CompareSeccompResult;

}  // namespace howdy::native::compare_sandbox_internal
