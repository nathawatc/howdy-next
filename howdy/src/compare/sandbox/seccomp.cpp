#include "compare/sandbox.hpp"
#include "compare/sandbox/internal.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <vector>

#include <sched.h>
#include <unistd.h>

#include <sys/syscall.h>

#include <linux/audit.h>
#include <linux/bpf_common.h>
#include <linux/filter.h>
#include <linux/prctl.h>
#include <linux/seccomp.h>

namespace {

	using howdy::native::CompareSeccompResult;
	using howdy::native::CompareSeccompStatus;
	using howdy::native::compare_sandbox_internal::CompareSeccompDependencies;

#ifdef __x86_64__
	constexpr __u32 kExpectedAuditArch = AUDIT_ARCH_X86_64;
#elifdef __aarch64__
	constexpr __u32 kExpectedAuditArch = AUDIT_ARCH_AARCH64;
#elif defined(__riscv) && (__riscv_xlen == 64)
	constexpr __u32 kExpectedAuditArch = AUDIT_ARCH_RISCV64;
#else
#	define HOWDY_SECCOMP_UNSUPPORTED_ARCH 1
#endif

	// ioctl request layout: direction and size in the upper 16 bits, then type, then number.
	constexpr __u32 kIoctlDirectionSizeMask = 0xffff0000U;
	constexpr __u32 kIoctlTypeMask          = 0x0000ff00U;
	constexpr __u32 kIoctlTypeShift         = 8;
	constexpr __u32 kIoctlTypeV4l2          = static_cast<__u32>('V') << kIoctlTypeShift;
	constexpr __u32 kIoctlTypeUvc           = static_cast<__u32>('u') << kIoctlTypeShift;
	constexpr __u32 kIoctlTcgets            = 0x5401U;
	// _IOR('T', 0x2A, struct termios2), spelled out so no termios header is needed; the
	// structure is 44 bytes on every architecture this filter supports.
	constexpr __u32 kIoctlTcgets2    = 0x802C542AU;
	constexpr __u32 kIoctlTiocgwinsz = 0x5413U;
	// Read-only requests stdio uses to ask whether a descriptor is a terminal.
	constexpr auto kIsattyIoctls = std::to_array<__u32>({
	    kIoctlTcgets,
	    kIoctlTcgets2,
	    kIoctlTiocgwinsz,
	});
	// Instructions in the ioctl block after its syscall-number test: the request load, an
	// allow pair per isatty request, then the direction test, type mask, two type tests, and
	// the allow and deny returns. Derived so the block's skip offset cannot drift from its size.
	constexpr std::size_t kIoctlTypeCheckInstructions = 6;
	constexpr std::size_t kIoctlBlockInstructions =
	    1 + (kIsattyIoctls.size() * 2) + kIoctlTypeCheckInstructions;

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
		filter.reserve(24 + kIoctlBlockInstructions + (allowed_syscalls.size() * 2));

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

#	ifdef SYS_ioctl
		// Compare inherits the PAM application's stdio, which is usually the user's terminal.
		// An unrestricted ioctl would let a compromised worker inject input (TIOCSTI,
		// TIOCLINUX), change the line discipline or terminal settings, or switch consoles
		// (VT_*, KD*). Permit only what the scan needs, by ioctl type instead of by request
		// number so camera drivers stay free to add controls: V4L2 ('V') and UVC ('u')
		// requests, which always encode a direction and size, plus the read-only stdio isatty
		// queries (TCGETS, TCGETS2 since glibc 2.42, TIOCGWINSZ on musl). Everything else
		// fails with EPERM: bare legacy requests such as VT_ACTIVATE share the 'V' type but
		// carry no direction bits. The filter cannot see which descriptor a request targets,
		// so the permitted types are allowed on every inherited descriptor, not only the camera.
		filter.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, static_cast<__u32>(SYS_ioctl), 0,
		                          static_cast<__u8>(kIoctlBlockInstructions)));
		filter.push_back(BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
		                          static_cast<__u32>(offsetof(struct seccomp_data, args[1]))));
		for (const __u32 isatty_request : kIsattyIoctls) {
			filter.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, isatty_request, 0, 1));
			filter.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
		}
		filter.push_back(BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, kIoctlDirectionSizeMask, 0, 4));
		filter.push_back(BPF_STMT(BPF_ALU | BPF_AND | BPF_K, kIoctlTypeMask));
		filter.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, kIoctlTypeV4l2, 1, 0));
		filter.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, kIoctlTypeUvc, 0, 1));
		filter.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
		filter.push_back(BPF_STMT(
		    BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (static_cast<__u32>(EPERM) & SECCOMP_RET_DATA)));
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

#	ifdef SYS_clone3
		// glibc pthread_create probes clone3 first. Seccomp cannot inspect pointer-backed
		// clone_args in cBPF. Returning ENOSYS causes glibc to fall back cleanly to clone(2)
		// where flags can be verified.
		filter.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, static_cast<__u32>(SYS_clone3), 0, 1));
		filter.push_back(BPF_STMT(
		    BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (static_cast<__u32>(ENOSYS) & SECCOMP_RET_DATA)));
#	endif

#	ifdef SYS_clone
		// Permit thread creation (e.g. OpenCV parallel worker pool, std::jthread) while
		// forbidding process creation (fork without CLONE_THREAD).
		filter.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, static_cast<__u32>(SYS_clone), 0, 4));
		filter.push_back(BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
		                          static_cast<__u32>(offsetof(struct seccomp_data, args[0]))));
		filter.push_back(
		    BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, static_cast<__u32>(CLONE_THREAD), 0, 1));
		filter.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
		filter.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS));
#	endif

		filter.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS));
		return filter;
#endif
	}

}  // namespace howdy::native::compare_sandbox_internal

namespace howdy::native::compare_sandbox_internal {

	auto ApplyCompareSeccompFilter(const CompareSeccompDependencies &dependencies)
	    -> CompareSeccompResult {
		if (dependencies.seccomp == nullptr) {
			return {
			    .status       = CompareSeccompStatus::kInvalidDependencies,
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

	auto ApplyCompareSeccompFilter() -> CompareSeccompResult {
		return compare_sandbox_internal::ApplyCompareSeccompFilter({
		    .context = nullptr,
		    .seccomp = SystemSeccomp,
		});
	}

}  // namespace howdy::native
