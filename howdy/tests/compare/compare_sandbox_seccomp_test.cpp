#include "compare/compare_sandbox_test_groups.hpp"
#include "compare/sandbox.hpp"
#include "compare/sandbox/internal.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <span>
#include <vector>

#include <sched.h>

#include <sys/syscall.h>

#include <linux/audit.h>
#include <linux/bpf_common.h>
#include <linux/filter.h>
#include <linux/prctl.h>
#include <linux/seccomp.h>

namespace {

	using howdy::test::Expect;

	using howdy::native::CompareSeccompStatus;
	using howdy::native::compare_sandbox_internal::kSeccompFilterFlagTsync;
	using howdy::native::compare_sandbox_internal::kSeccompRetData;
	using howdy::native::compare_sandbox_internal::kSeccompRetErrno;
	using howdy::native::compare_sandbox_internal::kSeccompRetKillProcess;
	using howdy::native::compare_sandbox_internal::kSeccompSetModeFilter;

#ifndef SYS_seccomp
#	define HOWDY_SECCOMP_UNSUPPORTED_ARCH 1
#elifdef __x86_64__
	constexpr __u32 kExpectedAuditArch = AUDIT_ARCH_X86_64;
#elif defined(__aarch64__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
	constexpr __u32 kExpectedAuditArch = AUDIT_ARCH_AARCH64;
#elif defined(__riscv) && (__riscv_xlen == 64) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
	constexpr __u32 kExpectedAuditArch = AUDIT_ARCH_RISCV64;
#else
#	define HOWDY_SECCOMP_UNSUPPORTED_ARCH 1
#endif

	struct FakeSeccompContext {
		unsigned int                    operation = 0;
		unsigned int                    flags     = 0;
		struct sock_fprog               prog{};
		std::vector<struct sock_filter> recorded_filter;
		int                             forced_errno = 0;
		int                             stale_errno  = 0;
		int                             return_code  = 0;
		// Errno for the null-program filter-support probe; EFAULT models a filter-capable
		// kernel.
		int probe_errno = EFAULT;
		int probe_calls = 0;
	};

	// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
	auto FakeSeccomp(void *raw_context, unsigned int operation, unsigned int flags, void *args)
	    -> int {
		auto &context = *static_cast<FakeSeccompContext *>(raw_context);
		if (args == nullptr) {
			++context.probe_calls;
			if (operation != kSeccompSetModeFilter || flags != 0) {
				errno = EINVAL;
				return -1;
			}
			errno = context.probe_errno;
			return context.probe_errno == 0 ? 0 : -1;
		}
		context.operation = operation;
		context.flags     = flags;
		if (args != nullptr) {
			context.prog = *static_cast<struct sock_fprog *>(args);
			if (context.prog.filter != nullptr && context.prog.len > 0) {
				const std::span filter_span(context.prog.filter, context.prog.len);
				context.recorded_filter.assign(filter_span.begin(), filter_span.end());
			}
		}
		if (context.forced_errno != 0) {
			errno = context.forced_errno;
			return -1;
		}
		if (context.stale_errno != 0) {
			errno = context.stale_errno;
		}
		return context.return_code;
	}

	auto TestFilterContainsArchitectureCheck() -> bool {
		const auto filter = howdy::native::compare_sandbox_internal::BuildCompareSeccompFilter();
#ifdef HOWDY_SECCOMP_UNSUPPORTED_ARCH
		return Expect(filter.empty(), "unsupported architecture constructs empty filter");
#else
		bool ok = true;
		ok &= Expect(!filter.empty(), "constructed seccomp filter is not empty");
		if (filter.size() < 4) {
			return false;
		}
		ok &= Expect(filter.at(0).code == (BPF_LD | BPF_W | BPF_ABS) &&
		                 filter.at(0).k == offsetof(struct seccomp_data, arch),
		             "filter instruction 0 loads seccomp_data arch");
		ok &= Expect(filter.at(1).code == (BPF_JMP | BPF_JEQ | BPF_K) &&
		                 filter.at(1).k == kExpectedAuditArch && filter.at(1).jt == 1 &&
		                 filter.at(1).jf == 0,
		             "filter instruction 1 validates expected AUDIT_ARCH_* constant");
		ok &= Expect(filter.at(2).code == (BPF_RET | BPF_K) &&
		                 filter.at(2).k == kSeccompRetKillProcess,
		             "filter instruction 2 kills process on architecture mismatch");
		ok &= Expect(filter.at(3).code == (BPF_LD | BPF_W | BPF_ABS) &&
		                 filter.at(3).k == offsetof(struct seccomp_data, nr),
		             "filter instruction 3 loads syscall nr");
		ok &= Expect(filter.back().code == (BPF_RET | BPF_K) &&
		                 filter.back().k == kSeccompRetKillProcess,
		             "filter final instruction fails closed with kill process");
		return ok;
#endif
	}

	auto TestFilterSyscallAllowlistContents() -> bool {
		const auto filter = howdy::native::compare_sandbox_internal::BuildCompareSeccompFilter();
#ifdef HOWDY_SECCOMP_UNSUPPORTED_ARCH
		return Expect(filter.empty(), "unsupported architecture has no allowlist rules");
#else
		std::vector<int> allowed_in_filter;
		for (std::size_t i = 4; i + 1 < filter.size(); i += 2) {
			if (filter.at(i).code == (BPF_JMP | BPF_JEQ | BPF_K) && filter.at(i).jt == 0 &&
			    filter.at(i).jf == 1 && filter.at(i + 1).code == (BPF_RET | BPF_K) &&
			    filter.at(i + 1).k == SECCOMP_RET_ALLOW) {
				allowed_in_filter.push_back(static_cast<int>(filter.at(i).k));
			} else {
				break;
			}
		}

		std::vector<int> expected_allowed = {
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
		std::ranges::sort(expected_allowed);
		const auto [last, end] = std::ranges::unique(expected_allowed);
		expected_allowed.erase(last, end);

		bool ok = true;
		for (const int allowed_nr : expected_allowed) {
			ok &= Expect(std::ranges::count(allowed_in_filter, allowed_nr) == 1,
			             "allow expected compare runtime syscall exactly once");
		}
		ok &= Expect(allowed_in_filter.size() == expected_allowed.size(),
		             "filter allowlist contains no unexpected syscalls");

		for (const int forbidden_nr : {
#	ifdef SYS_execve
		         SYS_execve,
#	endif
#	ifdef SYS_execveat
		         SYS_execveat,
#	endif
#	ifdef SYS_ptrace
		         SYS_ptrace,
#	endif
#	ifdef SYS_clone
		         SYS_clone,
#	endif
#	ifdef SYS_clone3
		         SYS_clone3,
#	endif
#	ifdef SYS_fork
		         SYS_fork,
#	endif
#	ifdef SYS_vfork
		         SYS_vfork,
#	endif
#	ifdef SYS_prctl
		         SYS_prctl,
#	endif
#	ifdef SYS_ioctl
		         SYS_ioctl,
#	endif
#	ifdef SYS_socket
		         SYS_socket,
#	endif
#	ifdef SYS_connect
		         SYS_connect,
#	endif
#	ifdef SYS_bind
		         SYS_bind,
#	endif
#	ifdef SYS_open
		         SYS_open,
#	endif
#	ifdef SYS_openat
		         SYS_openat,
#	endif
#	ifdef SYS_creat
		         SYS_creat,
#	endif
#	ifdef SYS_unlink
		         SYS_unlink,
#	endif
#	ifdef SYS_unlinkat
		         SYS_unlinkat,
#	endif
#	ifdef SYS_mremap
		         SYS_mremap,
#	endif
		     }) {
			ok &= Expect(std::ranges::count(allowed_in_filter, forbidden_nr) == 0,
			             "forbid non-runtime compare syscall");
		}
		return ok;
#endif
	}

	auto TestFilterPrctlArgumentFiltering() -> bool {
#if defined(SYS_prctl) && !defined(HOWDY_SECCOMP_UNSUPPORTED_ARCH)
		const auto  filter = howdy::native::compare_sandbox_internal::BuildCompareSeccompFilter();
		std::size_t prctl_index = filter.size();
		for (std::size_t i = 0; i < filter.size(); ++i) {
			if (filter.at(i).code == (BPF_JMP | BPF_JEQ | BPF_K) &&
			    filter.at(i).k == static_cast<__u32>(SYS_prctl)) {
				prctl_index = i;
				break;
			}
		}

		bool ok = true;
		ok &= Expect(prctl_index + 6 < filter.size(), "filter contains prctl argument block");
		if (prctl_index + 6 >= filter.size()) {
			return false;
		}

		ok &= Expect(filter.at(prctl_index).jt == 0 && filter.at(prctl_index).jf == 6,
		             "prctl jump skips argument block on mismatch");
		ok &= Expect(filter.at(prctl_index + 1).code == (BPF_LD | BPF_W | BPF_ABS) &&
		                 filter.at(prctl_index + 1).k ==
		                     static_cast<__u32>(offsetof(struct seccomp_data, args[0])),
		             "loads low 32-bit args[0]");
		ok &= Expect(filter.at(prctl_index + 2).code == (BPF_JMP | BPF_JEQ | BPF_K) &&
		                 filter.at(prctl_index + 2).k == static_cast<__u32>(PR_SET_NAME) &&
		                 filter.at(prctl_index + 2).jt == 0 && filter.at(prctl_index + 2).jf == 1,
		             "checks PR_SET_NAME");
		ok &= Expect(filter.at(prctl_index + 3).code == (BPF_RET | BPF_K) &&
		                 filter.at(prctl_index + 3).k == SECCOMP_RET_ALLOW,
		             "allows PR_SET_NAME");
		ok &= Expect(filter.at(prctl_index + 4).code == (BPF_JMP | BPF_JEQ | BPF_K) &&
		                 filter.at(prctl_index + 4).k == static_cast<__u32>(PR_GET_NAME) &&
		                 filter.at(prctl_index + 4).jt == 0 && filter.at(prctl_index + 4).jf == 1,
		             "checks PR_GET_NAME");
		ok &= Expect(filter.at(prctl_index + 5).code == (BPF_RET | BPF_K) &&
		                 filter.at(prctl_index + 5).k == SECCOMP_RET_ALLOW,
		             "allows PR_GET_NAME");
		ok &= Expect(filter.at(prctl_index + 6).code == (BPF_RET | BPF_K) &&
		                 filter.at(prctl_index + 6).k == kSeccompRetKillProcess,
		             "kills disallowed prctl option");
		return ok;
#else
		return true;
#endif
	}

	auto TestFilterIoctlArgumentFiltering() -> bool {
#if defined(SYS_ioctl) && !defined(HOWDY_SECCOMP_UNSUPPORTED_ARCH)
		const auto  filter = howdy::native::compare_sandbox_internal::BuildCompareSeccompFilter();
		std::size_t ioctl_index = filter.size();
		for (std::size_t i = 0; i < filter.size(); ++i) {
			if (filter.at(i).code == (BPF_JMP | BPF_JEQ | BPF_K) &&
			    filter.at(i).k == static_cast<__u32>(SYS_ioctl)) {
				ioctl_index = i;
				break;
			}
		}

		bool ok = true;
		ok &= Expect(ioctl_index + 13 < filter.size(), "filter contains ioctl argument block");
		if (ioctl_index + 13 >= filter.size()) {
			return false;
		}

		const auto is = [&](std::size_t offset, const struct sock_filter &expected) -> bool {
			const auto &actual = filter.at(ioctl_index + offset);
			return actual.code == expected.code && actual.k == expected.k &&
			       actual.jt == expected.jt && actual.jf == expected.jf;
		};
		constexpr __u32 denied = kSeccompRetErrno | (static_cast<__u32>(EPERM) & kSeccompRetData);
		constexpr __u32 allow_jump = BPF_JMP | BPF_JEQ | BPF_K;

		ok &= Expect(filter.at(ioctl_index).jt == 0 && filter.at(ioctl_index).jf == 13,
		             "ioctl jump skips argument block on mismatch");
		ok &= Expect(is(1, BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
		                            static_cast<__u32>(offsetof(struct seccomp_data, args[1])))),
		             "loads low 32-bit ioctl request");
		ok &= Expect(is(2, BPF_JUMP(allow_jump, howdy::test::compare_sandbox::kTcgets, 0, 1)),
		             "checks TCGETS");
		ok &= Expect(is(3, BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)), "allows TCGETS");
		ok &= Expect(is(4, BPF_JUMP(allow_jump, howdy::test::compare_sandbox::kTcgets2, 0, 1)),
		             "checks TCGETS2");
		ok &= Expect(is(5, BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)), "allows TCGETS2");
		ok &= Expect(is(6, BPF_JUMP(allow_jump, howdy::test::compare_sandbox::kTiocgwinsz, 0, 1)),
		             "checks TIOCGWINSZ");
		ok &= Expect(is(7, BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)), "allows TIOCGWINSZ");
		ok &= Expect(is(8, BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, 0xffff0000U, 0, 4)),
		             "bare legacy requests skip to denial");
		ok &= Expect(is(9, BPF_STMT(BPF_ALU | BPF_AND | BPF_K, 0x0000ff00U)),
		             "isolates ioctl type byte");
		ok &= Expect(is(10, BPF_JUMP(allow_jump, static_cast<__u32>('V') << 8U, 1, 0)),
		             "checks V4L2 ioctl type");
		ok &= Expect(is(11, BPF_JUMP(allow_jump, static_cast<__u32>('u') << 8U, 0, 1)),
		             "checks UVC ioctl type");
		ok &= Expect(is(12, BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)),
		             "allows camera ioctl types");
		ok &= Expect(is(13, BPF_STMT(BPF_RET | BPF_K, denied)),
		             "denies every other ioctl with EPERM");
		return ok;
#else
		return true;
#endif
	}

	auto TestFilterCloneArgumentFiltering() -> bool {
#ifndef HOWDY_SECCOMP_UNSUPPORTED_ARCH
		const auto filter = howdy::native::compare_sandbox_internal::BuildCompareSeccompFilter();
		bool       ok     = true;

#	ifdef SYS_clone3
		std::size_t clone3_index = filter.size();
		for (std::size_t i = 0; i < filter.size(); ++i) {
			if (filter.at(i).code == (BPF_JMP | BPF_JEQ | BPF_K) &&
			    filter.at(i).k == static_cast<__u32>(SYS_clone3)) {
				clone3_index = i;
				break;
			}
		}
		ok &= Expect(clone3_index + 1 < filter.size(), "filter contains clone3 block");
		if (clone3_index + 1 < filter.size()) {
			ok &= Expect(filter.at(clone3_index).jt == 0 && filter.at(clone3_index).jf == 1,
			             "clone3 jump skips errno return on mismatch");
			ok &=
			    Expect(filter.at(clone3_index + 1).code == (BPF_RET | BPF_K) &&
				           filter.at(clone3_index + 1).k ==
				               (kSeccompRetErrno | (static_cast<__u32>(ENOSYS) & kSeccompRetData)),
				       "clone3 returns ENOSYS errno");
		}
#	endif

#	ifdef SYS_clone
		std::size_t clone_index = filter.size();
		for (std::size_t i = 0; i < filter.size(); ++i) {
			if (filter.at(i).code == (BPF_JMP | BPF_JEQ | BPF_K) &&
			    filter.at(i).k == static_cast<__u32>(SYS_clone)) {
				clone_index = i;
				break;
			}
		}
		ok &= Expect(clone_index + 4 < filter.size(), "filter contains clone argument block");
		if (clone_index + 4 < filter.size()) {
			ok &= Expect(filter.at(clone_index).jt == 0 && filter.at(clone_index).jf == 4,
			             "clone jump skips argument block on mismatch");
			ok &= Expect(filter.at(clone_index + 1).code == (BPF_LD | BPF_W | BPF_ABS) &&
			                 filter.at(clone_index + 1).k ==
			                     static_cast<__u32>(offsetof(struct seccomp_data, args[0])),
			             "loads low 32-bit args[0]");
			ok &=
			    Expect(filter.at(clone_index + 2).code == (BPF_JMP | BPF_JSET | BPF_K) &&
				           filter.at(clone_index + 2).k == static_cast<__u32>(CLONE_THREAD) &&
				           filter.at(clone_index + 2).jt == 0 && filter.at(clone_index + 2).jf == 1,
				       "checks CLONE_THREAD");
			ok &= Expect(filter.at(clone_index + 3).code == (BPF_RET | BPF_K) &&
			                 filter.at(clone_index + 3).k == SECCOMP_RET_ALLOW,
			             "allows CLONE_THREAD");
			ok &= Expect(filter.at(clone_index + 4).code == (BPF_RET | BPF_K) &&
			                 filter.at(clone_index + 4).k == kSeccompRetKillProcess,
			             "kills clone without CLONE_THREAD");
		}
#	endif

		return ok;
#else
		return true;
#endif
	}

#ifdef HOWDY_SECCOMP_UNSUPPORTED_ARCH
	auto TestApplySeccompUnsupportedBuild() -> bool {
		bool ok = true;
		for (const int error_code : {0, ENOSYS, EINVAL, EPERM}) {
			FakeSeccompContext context;
			context.forced_errno = error_code;
			const auto result = howdy::native::compare_sandbox_internal::ApplyCompareSeccompFilter({
			    .context = &context,
			    .seccomp = FakeSeccomp,
			});
			ok &= Expect(result.status == CompareSeccompStatus::kUnsupportedArchitecture &&
			                 result.error_number == 0,
			             "build without a filter reports kUnsupportedArchitecture");
			ok &= Expect(context.prog.len == 0 && context.probe_calls == 0,
			             "build without a filter never calls seccomp");
		}
		return ok;
	}
#else
	auto TestApplySeccompSuccess() -> bool {
		FakeSeccompContext context;
		const auto result = howdy::native::compare_sandbox_internal::ApplyCompareSeccompFilter({
		    .context = &context,
		    .seccomp = FakeSeccomp,
		});
		bool       ok     = true;
		ok &= Expect(result.status == CompareSeccompStatus::kOk, "seccomp apply succeeds");
		ok &= Expect(result.error_number == 0, "seccomp apply error number is 0");
		ok &= Expect(context.operation == kSeccompSetModeFilter,
		             "operation is SECCOMP_SET_MODE_FILTER");
		ok &= Expect(context.flags == kSeccompFilterFlagTsync,
		             "flags include SECCOMP_FILTER_FLAG_TSYNC");
		ok &= Expect(context.prog.len > 0 && context.prog.filter != nullptr,
		             "valid sock_fprog passed");
		ok &= Expect(!context.recorded_filter.empty(), "filter instructions were passed");
		return ok;
	}

	auto TestApplySeccompUnsupportedKernel() -> bool {
		bool ok = true;
		for (const int error_code : {ENOSYS, ENOTSUP, EOPNOTSUPP}) {
			FakeSeccompContext context;
			context.forced_errno = error_code;
			const auto result = howdy::native::compare_sandbox_internal::ApplyCompareSeccompFilter({
			    .context = &context,
			    .seccomp = FakeSeccomp,
			});
			ok &= Expect(result.status == CompareSeccompStatus::kUnsupported,
			             "unsupported errno maps to kUnsupported");
			ok &= Expect(result.error_number == error_code, "preserves unsupported errno");
		}
		return ok;
	}

	auto TestApplySeccompInstallationFailure() -> bool {
		bool ok = true;
		for (const int error_code : {EINVAL, EPERM, EACCES, EFAULT}) {
			FakeSeccompContext context;
			context.forced_errno = error_code;
			const auto result = howdy::native::compare_sandbox_internal::ApplyCompareSeccompFilter({
			    .context = &context,
			    .seccomp = FakeSeccomp,
			});
			ok &= Expect(result.status == CompareSeccompStatus::kInstallationFailure,
			             "installation errno maps to kInstallationFailure");
			ok &= Expect(result.error_number == error_code, "preserves installation errno");
		}
		{
			FakeSeccompContext context;
			context.stale_errno = ENOSYS;
			context.return_code = 1234;
			const auto result = howdy::native::compare_sandbox_internal::ApplyCompareSeccompFilter({
			    .context = &context,
			    .seccomp = FakeSeccomp,
			});
			ok &= Expect(result.status == CompareSeccompStatus::kInstallationFailure &&
			                 result.error_number == EAGAIN,
			             "positive TSYNC return fails closed with EAGAIN even with stale ENOSYS");
		}
		return ok;
	}

	auto TestApplySeccompFilterModeUnavailable() -> bool {
		bool ok = true;
		{
			FakeSeccompContext context;
			context.forced_errno = EINVAL;
			context.probe_errno  = EINVAL;
			const auto result = howdy::native::compare_sandbox_internal::ApplyCompareSeccompFilter({
			    .context = &context,
			    .seccomp = FakeSeccomp,
			});
			ok &= Expect(result.status == CompareSeccompStatus::kUnsupported &&
			                 result.error_number == EINVAL && context.probe_calls == 1,
			             "kernel without CONFIG_SECCOMP_FILTER maps to kUnsupported");
		}
		for (const int probe_errno : {EFAULT, EPERM, EACCES}) {
			FakeSeccompContext context;
			context.forced_errno = EINVAL;
			context.probe_errno  = probe_errno;
			const auto result = howdy::native::compare_sandbox_internal::ApplyCompareSeccompFilter({
			    .context = &context,
			    .seccomp = FakeSeccomp,
			});
			ok &= Expect(result.status == CompareSeccompStatus::kInstallationFailure &&
			                 result.error_number == EINVAL,
			             "EINVAL on a filter-capable kernel stays an installation failure");
		}
		{
			FakeSeccompContext context;
			context.forced_errno = EPERM;
			context.probe_errno  = EINVAL;
			static_cast<void>(howdy::native::compare_sandbox_internal::ApplyCompareSeccompFilter({
			    .context = &context,
			    .seccomp = FakeSeccomp,
			}));
			ok &= Expect(context.probe_calls == 0,
			             "filter-support probe only runs after an EINVAL installation failure");
		}
		return ok;
	}

#endif

	auto TestApplySeccompNullDependencyFails() -> bool {
		const auto result = howdy::native::compare_sandbox_internal::ApplyCompareSeccompFilter({
		    .context = nullptr,
		    .seccomp = nullptr,
		});
		bool       ok     = true;
		ok &= Expect(result.status == CompareSeccompStatus::kInvalidDependencies,
		             "null seccomp callback fails closed");
		ok &= Expect(result.error_number == EINVAL, "null seccomp callback reports EINVAL");
		return ok;
	}

}  // namespace

auto RunCompareSandboxSeccompTests() -> bool {
	bool ok = true;
	ok &= TestFilterContainsArchitectureCheck();
	ok &= TestFilterSyscallAllowlistContents();
	ok &= TestFilterPrctlArgumentFiltering();
	ok &= TestFilterIoctlArgumentFiltering();
	ok &= TestFilterCloneArgumentFiltering();
#ifdef HOWDY_SECCOMP_UNSUPPORTED_ARCH
	ok &= TestApplySeccompUnsupportedBuild();
#else
	ok &= TestApplySeccompSuccess();
	ok &= TestApplySeccompUnsupportedKernel();
	ok &= TestApplySeccompInstallationFailure();
	ok &= TestApplySeccompFilterModeUnavailable();
#endif
	ok &= TestApplySeccompNullDependencyFails();
	ok &= RunCompareSandboxSeccompExecTests();
	return ok;
}
