#include "compare/sandbox.hpp"

#include "compare/sandbox/internal.hpp"
#include "support/prctl_types.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <vector>

#include <unistd.h>

#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>

#include <linux/audit.h>
#include <linux/bpf_common.h>
#include <linux/filter.h>
#include <linux/prctl.h>
#include <linux/seccomp.h>

namespace {

	using howdy::native::CompareSandboxResource;
	using howdy::native::CompareSandboxResult;
	using howdy::native::CompareSandboxStatus;
	using howdy::native::CompareSeccompResult;
	using howdy::native::CompareSeccompStatus;
	using howdy::native::compare_sandbox_internal::CompareSandboxDependencies;
	using howdy::native::compare_sandbox_internal::CompareSeccompDependencies;
	using howdy::native::compare_sandbox_internal::RlimitResource;

#ifdef __x86_64__
	constexpr __u32 kExpectedAuditArch = AUDIT_ARCH_X86_64;
#elifdef __aarch64__
	constexpr __u32 kExpectedAuditArch = AUDIT_ARCH_AARCH64;
#elif defined(__riscv) && (__riscv_xlen == 64)
	constexpr __u32 kExpectedAuditArch = AUDIT_ARCH_RISCV64;
#else
#	define HOWDY_SECCOMP_UNSUPPORTED_ARCH 1
#endif

	constexpr rlim_t kPreferredOpenFiles = 32;
	// Measured peak is five descriptors; 16 leaves room beyond stdio, camera, config/model files,
	// and OpenCV's internal descriptors.
	constexpr rlim_t kMinimumOpenFiles = 16;
	constexpr rlim_t kPreferredAddressSpace =
	    static_cast<rlim_t>(5ULL * 1024ULL * 1024ULL * 1024ULL / 2ULL);
	// Measured VmPeak is 1.672 GiB. Keep a conservative 2 GiB policy floor.
	constexpr rlim_t kMinimumAddressSpace = static_cast<rlim_t>(2ULL * 1024ULL * 1024ULL * 1024ULL);
	constexpr rlim_t kCpuSoftMargin       = 5;
	constexpr rlim_t kCpuHardMargin       = 10;
	constexpr rlim_t kCpuSoftFloor        = 15;
	constexpr rlim_t kCpuHardFloor        = 20;
	constexpr rlim_t kMaximumFiniteLimit  = RLIM_INFINITY - static_cast<rlim_t>(1);

	struct LimitPolicy {
		RlimitResource         system_resource;
		CompareSandboxResource resource;
		rlim_t                 preferred_soft;
		rlim_t                 preferred_hard;
		rlim_t                 minimum_soft;
		rlim_t                 minimum_hard;
	};

	auto FiniteMin(rlim_t preferred, rlim_t inherited) -> rlim_t {
		if (inherited == RLIM_INFINITY) {
			return preferred;
		}
		return std::min(preferred, inherited);
	}

	struct TimeoutLimit {
		int    seconds = 0;
		rlim_t margin  = 0;
	};

	auto ComputeTimeoutLimit(TimeoutLimit limit) -> rlim_t {
		const auto timeout =
		    limit.seconds > 0 ? static_cast<rlim_t>(limit.seconds) : static_cast<rlim_t>(0);
		if (timeout > kMaximumFiniteLimit - limit.margin) {
			return kMaximumFiniteLimit;
		}
		return timeout + limit.margin;
	}

	auto ApplyLimit(const LimitPolicy &policy, const CompareSandboxDependencies &dependencies)
	    -> CompareSandboxResult {
		rlimit inherited{};
		if (dependencies.getrlimit(dependencies.context, policy.system_resource, &inherited) != 0) {
			return {
			    .status       = CompareSandboxStatus::kLimitInspectionFailure,
			    .resource     = policy.resource,
			    .error_number = errno,
			};
		}

		const rlim_t target_hard = FiniteMin(policy.preferred_hard, inherited.rlim_max);
		const rlim_t target_soft =
		    FiniteMin(FiniteMin(policy.preferred_soft, inherited.rlim_cur), target_hard);
		if (target_soft < policy.minimum_soft || target_hard < policy.minimum_hard) {
			return {
			    .status       = CompareSandboxStatus::kLimitBelowMinimum,
			    .resource     = policy.resource,
			    .error_number = 0,
			};
		}

		const rlimit target{
		    .rlim_cur = target_soft,
		    .rlim_max = target_hard,
		};
		if (dependencies.setrlimit(dependencies.context, policy.system_resource, &target) != 0) {
			return {
			    .status       = CompareSandboxStatus::kLimitApplicationFailure,
			    .resource     = policy.resource,
			    .error_number = errno,
			};
		}
		return {};
	}

	auto SystemGetrlimit([[maybe_unused]] void *context, RlimitResource resource, rlimit *limit)
	    -> int {
		return getrlimit(resource, limit);
	}

	auto SystemSetrlimit([[maybe_unused]] void *context, RlimitResource resource,
	                     const rlimit *limit) -> int {
		return setrlimit(resource, limit);
	}

	auto SystemPrctl([[maybe_unused]] void *context, int operation,
	                 howdy::native::PrctlArgument argument2, howdy::native::PrctlArgument argument3,
	                 howdy::native::PrctlArgument argument4, howdy::native::PrctlArgument argument5)
	    -> int {
		return prctl(operation, argument2, argument3, argument4, argument5);
	}

	auto SystemSeccomp([[maybe_unused]] void *context, unsigned int operation, unsigned int flags,
	                   void *args) -> int {
#if defined(__linux__) && defined(SYS_seccomp)
		return static_cast<int>(syscall(SYS_seccomp, operation, flags, args));
#else
		errno = ENOSYS;
		return -1;
#endif
	}

}  // namespace

namespace howdy::native::compare_sandbox_internal {

	auto ApplyCompareSandbox(int timeout_seconds, const CompareSandboxDependencies &dependencies)
	    -> CompareSandboxResult {
		if (dependencies.prctl == nullptr || dependencies.getrlimit == nullptr ||
		    dependencies.setrlimit == nullptr) {
			return {
			    .status       = CompareSandboxStatus::kNoNewPrivilegesFailure,
			    .resource     = CompareSandboxResource::kNone,
			    .error_number = EINVAL,
			};
		}

		if (dependencies.prctl(dependencies.context, PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
			return {
			    .status       = CompareSandboxStatus::kNoNewPrivilegesFailure,
			    .resource     = CompareSandboxResource::kNone,
			    .error_number = errno,
			};
		}

		const rlim_t minimum_cpu =
		    ComputeTimeoutLimit({.seconds = timeout_seconds, .margin = kCpuSoftMargin});
		const std::array<LimitPolicy, 4> policies{
		    {
		        {
		            .system_resource = RLIMIT_CPU,
		            .resource        = CompareSandboxResource::kCpu,
		            .preferred_soft  = std::max(minimum_cpu, kCpuSoftFloor),
		            .preferred_hard  = std::max(
		                ComputeTimeoutLimit({.seconds = timeout_seconds, .margin = kCpuHardMargin}),
		                kCpuHardFloor),
		            .minimum_soft = minimum_cpu,
		            .minimum_hard = minimum_cpu,
		        },
		        {
		            .system_resource = RLIMIT_NOFILE,
		            .resource        = CompareSandboxResource::kOpenFiles,
		            .preferred_soft  = kPreferredOpenFiles,
		            .preferred_hard  = kPreferredOpenFiles,
		            .minimum_soft    = kMinimumOpenFiles,
		            .minimum_hard    = kMinimumOpenFiles,
		        },
		        {
		            .system_resource = RLIMIT_CORE,
		            .resource        = CompareSandboxResource::kCore,
		            .preferred_soft  = 0,
		            .preferred_hard  = 0,
		            .minimum_soft    = 0,
		            .minimum_hard    = 0,
		        },
		        {
		            .system_resource = RLIMIT_AS,
		            .resource        = CompareSandboxResource::kAddressSpace,
		            .preferred_soft  = kPreferredAddressSpace,
		            .preferred_hard  = kPreferredAddressSpace,
		            .minimum_soft    = kMinimumAddressSpace,
		            .minimum_hard    = kMinimumAddressSpace,
		        },
		    },
		};

		for (const auto &policy : policies) {
			const auto result = ApplyLimit(policy, dependencies);
			if (result.status != CompareSandboxStatus::kOk) {
				return result;
			}
		}
		return {};
	}

	auto BuildCompareSeccompFilter() -> std::vector<struct sock_filter> {
#ifdef HOWDY_SECCOMP_UNSUPPORTED_ARCH
		return {};
#else
		std::vector<int> allowed_syscalls = {
#	ifdef SYS_read
		    SYS_read,
#	endif
#	ifdef SYS_write
		    SYS_write,
#	endif
#	ifdef SYS_writev
		    SYS_writev,
#	endif
#	ifdef SYS_close
		    SYS_close,
#	endif
#	ifdef SYS_ioctl
		    // Broad ioctl allowance is necessary for V4L2 streaming (e.g. VIDIOC_DQBUF,
		    // VIDIOC_QBUF, VIDIOC_QUERYBUF, VIDIOC_STREAMON/OFF, exposure restore VIDIOC_S_CTRL,
		    // VIDIOC_S_EXT_CTRLS, and vendor UVC controls across camera models) and stdio isatty
		    // queries (TCGETS). Restricting request numbers is brittle across kernel drivers.
		    SYS_ioctl,
#	endif
#	ifdef SYS_poll
		    SYS_poll,
#	endif
#	ifdef SYS_ppoll
		    SYS_ppoll,
#	endif
#	ifdef SYS_select
		    SYS_select,
#	endif
#	ifdef SYS__newselect
		    SYS__newselect,
#	endif
#	ifdef SYS_pselect6
		    SYS_pselect6,
#	endif
#	ifdef SYS_fcntl
		    SYS_fcntl,
#	endif
#	ifdef SYS_lseek
		    SYS_lseek,
#	endif
#	ifdef SYS_fstat
		    SYS_fstat,
#	endif
#	ifdef SYS_newfstatat
		    SYS_newfstatat,
#	endif
#	ifdef SYS_fstatat64
		    SYS_fstatat64,
#	endif
#	ifdef SYS_mmap
		    SYS_mmap,
#	endif
#	ifdef SYS_munmap
		    SYS_munmap,
#	endif
#	ifdef SYS_mprotect
		    SYS_mprotect,
#	endif
#	ifdef SYS_madvise
		    SYS_madvise,
#	endif
#	ifdef SYS_brk
		    SYS_brk,
#	endif
#	ifdef SYS_futex
		    SYS_futex,
#	endif
#	ifdef SYS_futex_waitv
		    SYS_futex_waitv,
#	endif
#	ifdef SYS_sched_yield
		    SYS_sched_yield,
#	endif
#	ifdef SYS_sched_getaffinity
		    SYS_sched_getaffinity,
#	endif
#	ifdef SYS_nanosleep
		    SYS_nanosleep,
#	endif
#	ifdef SYS_clock_nanosleep
		    SYS_clock_nanosleep,
#	endif
#	ifdef SYS_clock_gettime
		    SYS_clock_gettime,
#	endif
#	ifdef SYS_clock_gettime64
		    SYS_clock_gettime64,
#	endif
#	ifdef SYS_gettimeofday
		    SYS_gettimeofday,
#	endif
#	ifdef SYS_getpid
		    SYS_getpid,
#	endif
#	ifdef SYS_gettid
		    SYS_gettid,
#	endif
#	ifdef SYS_getuid
		    SYS_getuid,
#	endif
#	ifdef SYS_geteuid
		    SYS_geteuid,
#	endif
#	ifdef SYS_getgid
		    SYS_getgid,
#	endif
#	ifdef SYS_getegid
		    SYS_getegid,
#	endif
#	ifdef SYS_rt_sigaction
		    SYS_rt_sigaction,
#	endif
#	ifdef SYS_rt_sigprocmask
		    SYS_rt_sigprocmask,
#	endif
#	ifdef SYS_rt_sigreturn
		    SYS_rt_sigreturn,
#	endif
#	ifdef SYS_sigaltstack
		    SYS_sigaltstack,
#	endif
#	ifdef SYS_rseq
		    SYS_rseq,
#	endif
#	ifdef SYS_set_robust_list
		    SYS_set_robust_list,
#	endif
#	ifdef SYS_getrandom
		    SYS_getrandom,
#	endif
#	ifdef SYS_exit
		    SYS_exit,
#	endif
#	ifdef SYS_exit_group
		    SYS_exit_group,
#	endif
#	ifdef SYS_restart_syscall
		    SYS_restart_syscall,
#	endif
		};

		std::ranges::sort(allowed_syscalls);
		const auto [last, end] = std::ranges::unique(allowed_syscalls);
		allowed_syscalls.erase(last, end);

		std::vector<struct sock_filter> filter;
		filter.reserve(16 + (allowed_syscalls.size() * 2));

		filter.push_back(BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
		                          static_cast<__u32>(offsetof(struct seccomp_data, arch))));
		filter.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, kExpectedAuditArch, 1, 0));
		filter.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS));

		filter.push_back(BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
		                          static_cast<__u32>(offsetof(struct seccomp_data, nr))));

		for (const int syscall_nr : allowed_syscalls) {
			filter.push_back(
			    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, static_cast<__u32>(syscall_nr), 0, 1));
			filter.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
		}

#	ifdef SYS_openat
		// glibc malloc arena trimming (heap_trim -> __open_nocancel) probes
		// /proc/sys/vm/overcommit_memory when freeing non-main-arena allocations (such as
		// OpenCV DNN weights allocated on the model worker thread) and falls back cleanly on
		// negative fd. Return EACCES instead of killing the process.
		filter.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, static_cast<__u32>(SYS_openat), 0, 1));
		filter.push_back(BPF_STMT(
		    BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (static_cast<__u32>(EACCES) & SECCOMP_RET_DATA)));
#	endif
#	ifdef SYS_open
		filter.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, static_cast<__u32>(SYS_open), 0, 1));
		filter.push_back(BPF_STMT(
		    BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (static_cast<__u32>(EACCES) & SECCOMP_RET_DATA)));
#	endif

#	ifdef SYS_prctl
		// Narrow prctl argument filter: permit only thread naming operations (PR_SET_NAME,
		// PR_GET_NAME) via the 32-bit option argument in args[0].
		filter.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, static_cast<__u32>(SYS_prctl), 0, 6));
		filter.push_back(BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
		                          static_cast<__u32>(offsetof(struct seccomp_data, args[0]))));
		filter.push_back(
		    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, static_cast<__u32>(PR_SET_NAME), 0, 1));
		filter.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
		filter.push_back(
		    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, static_cast<__u32>(PR_GET_NAME), 0, 1));
		filter.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
		filter.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS));
#	endif

		filter.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS));
		return filter;
#endif
	}

	auto ApplyCompareSeccompFilter(const CompareSeccompDependencies &dependencies)
	    -> CompareSeccompResult {
		if (dependencies.seccomp == nullptr) {
			return {
			    .status       = CompareSeccompStatus::kInstallationFailure,
			    .error_number = EINVAL,
			};
		}

#ifdef HOWDY_SECCOMP_UNSUPPORTED_ARCH
		return {
		    .status       = CompareSeccompStatus::kUnsupported,
		    .error_number = ENOSYS,
		};
#else
		auto filter = BuildCompareSeccompFilter();
		if (filter.empty() || filter.size() > USHRT_MAX) {
			return {
			    .status       = CompareSeccompStatus::kFilterConstructionFailure,
			    .error_number = EINVAL,
			};
		}

		struct sock_fprog prog = {
		    .len    = 0,
		    .filter = filter.data(),
		};
		prog.len = static_cast<decltype(prog.len)>(filter.size());

		if (dependencies.seccomp(dependencies.context, SECCOMP_SET_MODE_FILTER,
		                         SECCOMP_FILTER_FLAG_TSYNC, &prog) != 0) {
			const int err = errno;
			if (err == ENOSYS || err == ENOTSUP
#	if EOPNOTSUPP != ENOTSUP
			    || err == EOPNOTSUPP
#	endif
			) {
				return {
				    .status       = CompareSeccompStatus::kUnsupported,
				    .error_number = err,
				};
			}
			return {
			    .status       = CompareSeccompStatus::kInstallationFailure,
			    .error_number = err,
			};
		}
		return {};
#endif
	}

}  // namespace howdy::native::compare_sandbox_internal

namespace howdy::native {

	auto ApplyCompareSandbox(int timeout_seconds) -> CompareSandboxResult {
		return compare_sandbox_internal::ApplyCompareSandbox(timeout_seconds,
		                                                     {
		                                                         .context   = nullptr,
		                                                         .getrlimit = SystemGetrlimit,
		                                                         .setrlimit = SystemSetrlimit,
		                                                         .prctl     = SystemPrctl,
		                                                     });
	}

	auto ApplyCompareSeccompFilter() -> CompareSeccompResult {
		return compare_sandbox_internal::ApplyCompareSeccompFilter({
		    .context = nullptr,
		    .seccomp = SystemSeccomp,
		});
	}

}  // namespace howdy::native
