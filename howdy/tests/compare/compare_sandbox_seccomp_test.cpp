#include "compare/compare_sandbox_test_groups.hpp"
#include "compare/sandbox.hpp"
#include "compare/sandbox/internal.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#include <linux/audit.h>
#include <linux/bpf_common.h>
#include <linux/filter.h>
#include <linux/prctl.h>
#include <linux/seccomp.h>

namespace {

	using howdy::test::Expect;

	using howdy::native::CompareSeccompStatus;

#ifdef __x86_64__
	constexpr __u32 kExpectedAuditArch = AUDIT_ARCH_X86_64;
#elifdef __aarch64__
	constexpr __u32 kExpectedAuditArch = AUDIT_ARCH_AARCH64;
#elif defined(__riscv) && (__riscv_xlen == 64)
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
		int                             return_code  = 0;
	};

	// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
	auto FakeSeccomp(void *raw_context, unsigned int operation, unsigned int flags, void *args)
	    -> int {
		auto &context     = *static_cast<FakeSeccompContext *>(raw_context);
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
		                 filter.at(2).k == SECCOMP_RET_KILL_PROCESS,
		             "filter instruction 2 kills process on architecture mismatch");
		ok &= Expect(filter.at(3).code == (BPF_LD | BPF_W | BPF_ABS) &&
		                 filter.at(3).k == offsetof(struct seccomp_data, nr),
		             "filter instruction 3 loads syscall nr");
		ok &= Expect(filter.back().code == (BPF_RET | BPF_K) &&
		                 filter.back().k == SECCOMP_RET_KILL_PROCESS,
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
#	ifdef SYS_ioctl
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
		                 filter.at(prctl_index + 6).k == SECCOMP_RET_KILL_PROCESS,
		             "kills disallowed prctl option");
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
			ok &= Expect(
			    filter.at(clone3_index + 1).code == (BPF_RET | BPF_K) &&
			        filter.at(clone3_index + 1).k ==
			            (SECCOMP_RET_ERRNO | (static_cast<__u32>(ENOSYS) & SECCOMP_RET_DATA)),
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
			                 filter.at(clone_index + 4).k == SECCOMP_RET_KILL_PROCESS,
			             "kills clone without CLONE_THREAD");
		}
#	endif

		return ok;
#else
		return true;
#endif
	}

	auto TestApplySeccompSuccess() -> bool {
		FakeSeccompContext context;
		const auto result = howdy::native::compare_sandbox_internal::ApplyCompareSeccompFilter({
		    .context = &context,
		    .seccomp = FakeSeccomp,
		});
		bool       ok     = true;
		ok &= Expect(result.status == CompareSeccompStatus::kOk, "seccomp apply succeeds");
		ok &= Expect(result.error_number == 0, "seccomp apply error number is 0");
		ok &= Expect(context.operation == SECCOMP_SET_MODE_FILTER,
		             "operation is SECCOMP_SET_MODE_FILTER");
		ok &= Expect(context.flags == SECCOMP_FILTER_FLAG_TSYNC,
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
		return ok;
	}

	auto TestApplySeccompNullDependencyFails() -> bool {
		const auto result = howdy::native::compare_sandbox_internal::ApplyCompareSeccompFilter({
		    .context = nullptr,
		    .seccomp = nullptr,
		});
		bool       ok     = true;
		ok &= Expect(result.status == CompareSeccompStatus::kInstallationFailure,
		             "null seccomp callback fails closed");
		ok &= Expect(result.error_number == EINVAL, "null seccomp callback reports EINVAL");
		return ok;
	}

	auto RunSeccompChildProbe(std::string_view action) -> int {
		constexpr std::size_t non_main_arena_block_size = 256UZ * 1024UZ;
		void                 *non_main_arena_block      = nullptr;
		std::thread           arena_worker([&non_main_arena_block] -> void {
			non_main_arena_block = malloc(non_main_arena_block_size);
			if (non_main_arena_block != nullptr) {
				std::memset(non_main_arena_block, 0x5A, non_main_arena_block_size);
			}
		});
		arena_worker.join();
		if (non_main_arena_block == nullptr) {
			return 1;
		}

		const auto sandbox_result = howdy::native::ApplyCompareSandbox(1);
		if (sandbox_result.status != howdy::native::CompareSandboxStatus::kOk) {
			free(non_main_arena_block);
			return 1;
		}
		const auto seccomp_result = howdy::native::ApplyCompareSeccompFilter();
		if (seccomp_result.status == howdy::native::CompareSeccompStatus::kUnsupported) {
			free(non_main_arena_block);
			return 77;
		}
		if (seccomp_result.status != howdy::native::CompareSeccompStatus::kOk) {
			free(non_main_arena_block);
			return 2;
		}

		if (action == "allowed") {
			// Exercise glibc non-main-arena trimming (heap_trim -> __open_nocancel), which
			// probes /proc/sys/vm/overcommit_memory when freeing large worker-thread allocations
			// such as OpenCV DNN model weights during FaceModel destruction.
			free(non_main_arena_block);
			errno = 0;
			if (open("/proc/sys/vm/overcommit_memory", O_RDONLY | O_CLOEXEC) != -1 ||
			    errno != EACCES) {
				return 6;
			}
			if (getpid() <= 0) {
				return 3;
			}
#ifdef PR_SET_NAME
			if (prctl(PR_SET_NAME, "probe", 0, 0, 0) != 0) {
				return 4;
			}
#endif
#ifdef PR_GET_NAME
			std::array<char, 16> name_buffer{};
			if (prctl(PR_GET_NAME, name_buffer.data(), 0, 0, 0) != 0) {
				return 5;
			}
#endif
#ifdef SYS_clone3
			errno = 0;
			if (syscall(SYS_clone3, nullptr, 0) != -1 || errno != ENOSYS) {
				return 7;
			}
#endif
			std::thread thread_probe([] -> void {});
			thread_probe.join();
			return 0;
		}
		free(non_main_arena_block);
		if (action == "bad-prctl") {
#ifdef PR_SET_DUMPABLE
			(void)prctl(PR_SET_DUMPABLE, 1, 0, 0, 0);
#endif
			return 0;
		}
		if (action == "bad-syscall") {
#ifdef SYS_getpgid
			(void)syscall(SYS_getpgid, 0);
#elifdef SYS_uname
			(void)syscall(SYS_uname, nullptr);
#else
			(void)syscall(99999);
#endif
			return 0;
		}
		if (action == "bad-clone") {
#ifdef SYS_clone
			(void)syscall(SYS_clone, 0, nullptr, nullptr, nullptr, 0);
#elifdef SYS_fork
			(void)syscall(SYS_fork);
#endif
			return 0;
		}
		return 6;
	}

	auto RunSubprocessCheck(std::string_view action) -> std::optional<int> {
		std::string           program("howdy_compare_sandbox_test");
		std::string           mode("--seccomp-probe");
		std::string           action_arg(action);
		std::array<char *, 4> arguments = {program.data(), mode.data(), action_arg.data(), nullptr};
		// Pass an empty environment to prevent inherited LD_PRELOAD, sanitizer, or runtime
		// variables from executing extra syscalls or spawning threads before seccomp installation.
		std::array<char *, 1> environment = {nullptr};
		pid_t                 pid         = -1;
		if (posix_spawn(&pid, "/proc/self/exe", nullptr, nullptr, arguments.data(),
		                environment.data()) != 0) {
			return std::nullopt;
		}
		int status = 0;
		while (waitpid(pid, &status, 0) < 0) {
			if (errno != EINTR) {
				return std::nullopt;
			}
		}
		return status;
	}

	auto TestLiveSeccompEnforcementInSubprocess() -> bool {
		// Positive path: allowed getpid and allowed prctl thread naming operations
		const auto allowed_status = RunSubprocessCheck("allowed");
		if (!allowed_status.has_value()) {
			return Expect(false, "spawn and reap allowed seccomp probe");
		}

		if (WIFEXITED(*allowed_status) && WEXITSTATUS(*allowed_status) == 77) {
			return Expect(true, "live seccomp skipped on unsupported kernel");
		}

		bool ok = true;
		ok &= Expect(WIFEXITED(*allowed_status) && WEXITSTATUS(*allowed_status) == 0,
		             "allowed syscalls and prctl naming succeed under real filter");

		const auto expect_killed_by_seccomp = [&](const std::optional<int> &status,
		                                          const char               *label) -> bool {
			if (!status.has_value()) {
				return Expect(false, std::string("spawn and reap ") + label);
			}
			bool killed = Expect(WIFSIGNALED(*status), label);
#ifdef SIGSYS
			killed &= Expect(WTERMSIG(*status) == SIGSYS || WTERMSIG(*status) == SIGKILL, label);
#endif
			return killed;
		};

		// Negative path 1: disallowed prctl (PR_SET_DUMPABLE)
		const auto bad_prctl_status = RunSubprocessCheck("bad-prctl");
		ok &= expect_killed_by_seccomp(bad_prctl_status, "disallowed prctl was killed by seccomp");

		// Negative path 2: disallowed syscall (getpgid / uname)
		const auto bad_syscall_status = RunSubprocessCheck("bad-syscall");
		ok &= expect_killed_by_seccomp(bad_syscall_status,
		                               "disallowed syscall was killed by seccomp");

		// Negative path 3: disallowed clone / clone3 post-seccomp
		const auto bad_clone_status = RunSubprocessCheck("bad-clone");
		ok &= expect_killed_by_seccomp(bad_clone_status,
		                               "disallowed clone/clone3 was killed by seccomp");
		return ok;
	}

}  // namespace

auto RunCompareSandboxSeccompProbe(std::string_view action) -> int {
	return RunSeccompChildProbe(action);
}

auto RunCompareSandboxSeccompTests() -> bool {
	bool ok = true;
	ok &= TestFilterContainsArchitectureCheck();
	ok &= TestFilterSyscallAllowlistContents();
	ok &= TestFilterPrctlArgumentFiltering();
	ok &= TestFilterCloneArgumentFiltering();
	ok &= TestApplySeccompSuccess();
	ok &= TestApplySeccompUnsupportedKernel();
	ok &= TestApplySeccompInstallationFailure();
	ok &= TestApplySeccompNullDependencyFails();
	ok &= TestLiveSeccompEnforcementInSubprocess();
	return ok;
}
