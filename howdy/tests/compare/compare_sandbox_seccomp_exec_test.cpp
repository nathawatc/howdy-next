#include "compare/compare_sandbox_test_groups.hpp"
#include "compare/sandbox.hpp"
#include "compare/sandbox/internal.hpp"
#include "support/eintr.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#include <linux/bpf_common.h>
#include <linux/filter.h>
#include <linux/prctl.h>
#include <linux/seccomp.h>
#include <linux/uvcvideo.h>
#include <linux/videodev2.h>

namespace {

	using howdy::test::Expect;

	// Requests the filter lets through: isatty queries plus V4L2 and UVC camera requests.
	constexpr auto kPermittedIoctls = std::to_array<__u32>({
	    howdy::test::compare_sandbox::kTcgets,
	    howdy::test::compare_sandbox::kTcgets2,
	    howdy::test::compare_sandbox::kTiocgwinsz,
	    static_cast<__u32>(VIDIOC_QUERYCAP),
	    static_cast<__u32>(VIDIOC_DQBUF),
	    static_cast<__u32>(VIDIOC_S_EXT_CTRLS),
	    static_cast<__u32>(UVCIOC_CTRL_QUERY),
	});
	// Requests the filter must refuse on an inherited terminal: TIOCSTI, TIOCLINUX, TIOCSETD,
	// TCSETS, TCSETS2 (a terminal request that does carry direction bits), VT_ACTIVATE,
	// KDSETMODE, and FIONREAD.
	constexpr auto kRefusedIoctls = std::to_array<__u32>({
	    0x5412U,
	    0x541CU,
	    0x5423U,
	    0x5402U,
	    0x402C542BU,
	    0x5606U,
	    0x4B3AU,
	    0x541BU,
	});

	// /dev/null answers every ioctl with ENOTTY, so a request the filter lets through is told
	// apart from one it refuses with EPERM.
	auto ProbeIoctlFilter(int fd) -> int {
		const auto ioctl_errno = [fd](__u32 request) -> int {
			std::uint64_t argument = 0;
			errno                  = 0;
			return syscall(SYS_ioctl, fd, std::uint64_t{request}, &argument) == -1 ? errno : 0;
		};
		if (!std::ranges::all_of(kPermittedIoctls, [&](__u32 request) -> bool {
			    return ioctl_errno(request) == ENOTTY;
		    })) {
			return 8;
		}
		if (!std::ranges::all_of(kRefusedIoctls, [&](__u32 request) -> bool {
			    return ioctl_errno(request) == EPERM;
		    })) {
			return 9;
		}
		return 0;
	}

	auto SeccompUnavailable(howdy::native::CompareSeccompStatus status) -> bool {
		return status == howdy::native::CompareSeccompStatus::kUnsupported ||
		       status == howdy::native::CompareSeccompStatus::kUnsupportedArchitecture;
	}

	// mremap is denied without killing, so realloc of an mmapped block falls back to copying.
	auto ProbeMremapDenied() -> bool {
		constexpr std::size_t size = 1024UZ * 1024UZ;
		void                 *block =
		    mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (block == MAP_FAILED) {
			return false;
		}
		errno             = 0;
		const bool denied = mremap(block, size, size * 2, MREMAP_MAYMOVE) == MAP_FAILED &&
		                    errno == ENOMEM && munmap(block, size) == 0;
		auto      *bytes  = static_cast<unsigned char *>(malloc(size));
		if (bytes == nullptr) {
			return false;
		}
		std::memset(bytes, 0x5A, size);
		auto *grown = static_cast<unsigned char *>(realloc(bytes, size * 8));
		if (grown == nullptr) {
			free(bytes);
			return false;
		}
		const bool kept = std::span(grown, size).back() == 0x5A;
		free(grown);
		return denied && kept;
	}

	auto ProbeErrnoDenials(int ioctl_probe_fd) -> int {
		return ProbeMremapDenied() ? ProbeIoctlFilter(ioctl_probe_fd) : 10;
	}

	auto RunSeccompChildProbe(std::string_view action) -> int {
#ifdef PR_SET_DUMPABLE
		(void)prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
#endif
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

		// Opened before the filter, which refuses open afterwards.
		const int ioctl_probe_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
		if (ioctl_probe_fd < 0) {
			free(non_main_arena_block);
			return 1;
		}

		const auto sandbox_result = howdy::native::ApplyCompareSandbox(1);
		if (sandbox_result.status != howdy::native::CompareSandboxStatus::kOk) {
			free(non_main_arena_block);
			return 1;
		}
		const auto seccomp_result = howdy::native::ApplyCompareSeccompFilter();
		if (SeccompUnavailable(seccomp_result.status)) {
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

			if (const int denied_result = ProbeErrnoDenials(ioctl_probe_fd); denied_result != 0) {
				return denied_result;
			}
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
			return Expect(true, "live seccomp skipped on unsupported kernel or build");
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

#ifdef SYS_seccomp
	constexpr int kOuterFilterUnavailable = 77;

	// How an emulated kernel answers seccomp(2): always `error_number`, or, when it accepts filter
	// mode, only once it is handed a program (a null one reaches the real kernel and faults).
	struct EmulatedSeccomp {
		int  error_number        = 0;
		bool accepts_filter_mode = false;
	};

	// Puts the emulation in front of the production filter install. Never returns.
	[[noreturn]] void RunUnderEmulatedSeccomp(const EmulatedSeccomp &emulated) {
		const auto error_action =
		    static_cast<__u32>(SECCOMP_RET_ERRNO) | static_cast<__u32>(emulated.error_number);
		const auto null_action =
		    emulated.accepts_filter_mode ? static_cast<__u32>(SECCOMP_RET_ALLOW) : error_action;
		constexpr auto program_argument =
		    static_cast<__u32>(offsetof(struct seccomp_data, args) + (2 * sizeof(__u64)));
		constexpr auto    load   = static_cast<__u16>(BPF_LD | BPF_W | BPF_ABS);
		constexpr auto    match  = static_cast<__u16>(BPF_JMP | BPF_JEQ | BPF_K);
		constexpr auto    ret    = static_cast<__u16>(BPF_RET | BPF_K);
		constexpr auto    number = static_cast<__u32>(offsetof(struct seccomp_data, nr));
		auto              filter = std::to_array<struct sock_filter>({
		    {.code = load, .jt = 0, .jf = 0, .k = number},
		    {.code = match, .jt = 0, .jf = 3, .k = SYS_seccomp},
		    {.code = load, .jt = 0, .jf = 0, .k = program_argument},
		    {.code = match, .jt = 0, .jf = 2, .k = 0},
		    {.code = ret, .jt = 0, .jf = 0, .k = null_action},
		    {.code = ret, .jt = 0, .jf = 0, .k = SECCOMP_RET_ALLOW},
		    {.code = ret, .jt = 0, .jf = 0, .k = error_action},
		});
		struct sock_fprog program{
		    .len    = static_cast<__u16>(filter.size()),
		    .filter = filter.data(),
		};
		if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 ||
		    prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program, 0, 0) != 0) {
			_exit(kOuterFilterUnavailable);
		}
		_exit(static_cast<int>(howdy::native::ApplyCompareSeccompFilter().status));
	}

	auto ExpectEmulatedSeccomp(const EmulatedSeccomp              &emulated,
	                           howdy::native::CompareSeccompStatus expected,
	                           const std::string                  &message) -> bool {
		const pid_t child = fork();
		if (child == 0) {
			RunUnderEmulatedSeccomp(emulated);
		}
		int status = 0;
		if (child < 0 || howdy::native::RetryOnEintr([&] -> pid_t {
			                 return waitpid(child, &status, 0);
		                 }) != child) {
			return Expect(false, message + ": run emulated seccomp child");
		}
		if (WIFEXITED(status) && WEXITSTATUS(status) == kOuterFilterUnavailable) {
			std::cerr << "SKIP: " << message << " (no seccomp filter to emulate with)\n";
			return true;
		}
		return Expect(WIFEXITED(status) && WEXITSTATUS(status) == static_cast<int>(expected),
		              message + ", status=" + std::to_string(status));
	}

	// Only a kernel that has no seccomp, or no filter mode, may run compare unfiltered.
	auto TestInstallOnEmulatedKernels() -> bool {
		using howdy::native::CompareSeccompStatus;
		if (howdy::native::compare_sandbox_internal::BuildCompareSeccompFilter().empty()) {
			return Expect(true, "emulated kernels skipped on a build without a filter");
		}
		bool ok = true;
		ok &= ExpectEmulatedSeccomp({.error_number = ENOSYS}, CompareSeccompStatus::kUnsupported,
		                            "kernel without seccomp is unsupported");
		ok &= ExpectEmulatedSeccomp({.error_number = EINVAL}, CompareSeccompStatus::kUnsupported,
		                            "kernel without filter mode is unsupported");
		ok &= ExpectEmulatedSeccomp({.error_number = EINVAL, .accepts_filter_mode = true},
		                            CompareSeccompStatus::kInstallationFailure,
		                            "filter rejected by a filter-capable kernel is a failure");
		ok &= ExpectEmulatedSeccomp({.error_number = EPERM},
		                            CompareSeccompStatus::kInstallationFailure,
		                            "refused filter is a failure");
		return ok;
	}
#else
	auto TestInstallOnEmulatedKernels() -> bool {
		return Expect(true, "emulated kernels skipped without seccomp headers");
	}
#endif

}  // namespace

auto RunCompareSandboxSeccompProbe(std::string_view action) -> int {
	return RunSeccompChildProbe(action);
}

auto RunCompareSandboxSeccompExecTests() -> bool {
	bool ok = TestLiveSeccompEnforcementInSubprocess();
	ok &= TestInstallOnEmulatedKernels();
	return ok;
}
