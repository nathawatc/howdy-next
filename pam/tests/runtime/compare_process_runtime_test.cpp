#include "paths.hpp"
#include "protocol/compare_descriptors.hpp"
#include "protocol/compare_exit.hpp"
#include "protocol/compare_verdict.hpp"
#include "runtime/child_process.hpp"
#include "runtime/compare_launch.hpp"
#include "runtime/compare_process.hpp"
#include "runtime/compare_process_test_support.hpp"
#include "support/bit_flags.hpp"
#include "support/process_status.hpp"
#include "support/process_test_support.hpp"
#include "support/scoped_fd.hpp"
#include "test_support.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <sys/eventfd.h>
#include <sys/poll.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

namespace {
	struct WaitTestState {
		bool              fail_pidfd         = false;
		int               waitid_error       = 0;
		int               waitid_any_error   = 0;  // every waitid, by PID as well as by pidfd
		bool              fail_pidfd_signal  = false;
		int               pidfd_signal_error = 0;
		bool              fail_kill          = false;
		bool              signal_refused     = false;
		int               release_fd         = -1;  // closed by the first poll after a refusal
		pid_t             release_pid        = -1;
		int               last_pidfd         = -1;
		int               poll_error         = 0;
		bool              invalid_poll_event = false;
		std::atomic<bool> poll_entered{false};
		int               poll_calls     = 0;
		int               fail_poll_call = 0;
		int               term_calls     = 0;
		int               kill_calls     = 0;
		int               waitid_calls   = 0;
		int               waitpid_calls  = 0;
		int               reaped_status  = 0;
	};

	auto WaitState() -> WaitTestState & {
		static WaitTestState state;
		return state;
	}
}  // namespace

extern "C" auto TestPidfdOpen(pid_t pid, unsigned int flags) -> int __asm__("__wrap_pidfd_open");
extern "C" auto RealPidfdOpen(pid_t pid, unsigned int flags) -> int __asm__("__real_pidfd_open");
extern "C" auto TestPpoll(pollfd *fds, nfds_t count, const timespec *timeout, const sigset_t *mask)
    -> int __asm__("__wrap_ppoll");
extern "C" auto RealPpoll(pollfd *fds, nfds_t count, const timespec *timeout, const sigset_t *mask)
    -> int __asm__("__real_ppoll");
extern "C" auto TestPpollChecked(pollfd *fds, nfds_t count, const timespec *timeout,
                                 const sigset_t *mask, std::size_t buffer_size)
    -> int __asm__("__wrap___ppoll_chk");
extern "C" auto TestKill(pid_t pid, int signal) -> int __asm__("__wrap_kill");
extern "C" auto RealKill(pid_t pid, int signal) -> int __asm__("__real_kill");
extern "C" auto TestWaitpid(pid_t pid, int *status, int options) -> pid_t __asm__("__wrap_waitpid");
extern "C" auto RealWaitpid(pid_t pid, int *status, int options) -> pid_t __asm__("__real_waitpid");
extern "C" auto TestPidfdSendSignal(int pidfd, int signal, siginfo_t *info, unsigned int flags)
    -> int __asm__("__wrap_pidfd_send_signal");
extern "C" auto RealPidfdSendSignal(int pidfd, int signal, siginfo_t *info, unsigned int flags)
    -> int __asm__("__real_pidfd_send_signal");
extern "C" auto TestWaitid(idtype_t id_type, id_t id, siginfo_t *info, int options)
    -> int __asm__("__wrap_waitid");
extern "C" auto RealWaitid(idtype_t id_type, id_t id, siginfo_t *info, int options)
    -> int __asm__("__real_waitid");

auto TestPidfdOpen(pid_t pid, unsigned int flags) -> int {
	if (WaitState().fail_pidfd) {
		errno = ENOSYS;
		return -1;
	}
	WaitState().last_pidfd = RealPidfdOpen(pid, flags);
	return WaitState().last_pidfd;
}

auto TestPpoll(pollfd *fds, nfds_t count, const timespec *timeout, const sigset_t *mask) -> int {
	++WaitState().poll_calls;
	WaitState().poll_entered.store(true);
	if (WaitState().fail_poll_call == WaitState().poll_calls) {
		errno = EIO;
		return -1;
	}
	if (WaitState().poll_error != 0) {
		errno                  = WaitState().poll_error;
		WaitState().poll_error = 0;
		return -1;
	}
	if (WaitState().invalid_poll_event) {
		WaitState().invalid_poll_event = false;
		fds->revents                   = POLLNVAL;
		return 1;
	}
	if (WaitState().signal_refused && WaitState().release_fd >= 0) {
		close(std::exchange(WaitState().release_fd, -1));
		siginfo_t info{};
		(void)RealWaitid(P_PID, static_cast<id_t>(WaitState().release_pid), &info,
		                 howdy::native::CombineBitsAs<int>(WEXITED, WNOWAIT));
	}
	return RealPpoll(fds, count, timeout, mask);
}

auto TestPpollChecked(pollfd *fds, nfds_t count, const timespec *timeout, const sigset_t *mask,
                      [[maybe_unused]] std::size_t buffer_size) -> int {
	return TestPpoll(fds, count, timeout, mask);
}

namespace {
	void CountSignal(int signal) {
		if (signal == SIGTERM) {
			++WaitState().term_calls;
		}
		if (signal == SIGKILL) {
			++WaitState().kill_calls;
		}
	}
}  // namespace

auto TestKill(pid_t pid, int signal) -> int {
	if (WaitState().fail_kill) {
		WaitState().signal_refused = true;
		errno                      = EPERM;
		return -1;
	}
	CountSignal(signal);
	return RealKill(pid, signal);
}

auto TestPidfdSendSignal(int pidfd, int signal, siginfo_t *info, unsigned int flags) -> int {
	if (WaitState().pidfd_signal_error != 0) {
		errno = WaitState().pidfd_signal_error;
		return -1;
	}
	if (WaitState().fail_pidfd_signal) {
		errno = EPERM;
		return -1;
	}
	CountSignal(signal);
	return RealPidfdSendSignal(pidfd, signal, info, flags);
}

// Records what a pidfd reap consumed, in wait(2) form, ignoring peeks and empty polls.
auto TestWaitid(idtype_t id_type, id_t id, siginfo_t *info, int options) -> int {
	++WaitState().waitid_calls;
	if (WaitState().waitid_any_error != 0) {
		errno = WaitState().waitid_any_error;
		return -1;
	}
	if (id_type == P_PIDFD && WaitState().waitid_error != 0) {
		errno = WaitState().waitid_error;
		return -1;
	}
	const int result = RealWaitid(id_type, id, info, options);
	if (result == 0 && id_type == P_PIDFD && info != nullptr && info->si_pid != 0 &&
	    (static_cast<unsigned int>(options) & static_cast<unsigned int>(WNOWAIT)) == 0U) {
		const auto value = static_cast<unsigned int>(info->si_status);
		WaitState().reaped_status =
		    info->si_code == CLD_EXITED
		        ? static_cast<int>((value & 0xffU) << 8U)
		        : static_cast<int>((value & 0x7fU) | (info->si_code == CLD_DUMPED ? 0x80U : 0U));
	}
	return result;
}

auto TestWaitpid(pid_t pid, int *status, int options) -> pid_t {
	++WaitState().waitpid_calls;
	const pid_t result = RealWaitpid(pid, status, options);
	if (result == pid && status != nullptr) {
		WaitState().reaped_status = *status;
	}
	return result;
}

namespace {
	using howdy::test::process::SpawnChild;
	using std::chrono_literals::operator""ms;
	using std::chrono_literals::operator""s;
	using howdy::test::process::ChildReaped;
	using howdy::test::process::ScopedSignalBlock;
	using howdy::test::process::SpawnBlockedChild;
	using howdy::test::process::SpawnSigtermIgnoringChild;
	using howdy::test::prompt_coordinator::CompareRequestFiles;
	using howdy::test::prompt_coordinator::PosixSpawnCapture;
	using howdy::test::prompt_coordinator::TimeoutWaitStatus;

	using howdy::test::Expect;

	constexpr int kVerdictFd     = howdy::native::compare_verdict::kFd;
	constexpr int kConfigFd      = howdy::native::compare_descriptors::kConfigFd;
	constexpr int kModelFd       = howdy::native::compare_descriptors::kModelFd;
	constexpr int kFirstUnusedFd = howdy::native::compare_descriptors::kFirstUnusedFd;

	// The environment compare gets in descriptor mode, in order.
	auto DescriptorModeEnvironment() -> std::vector<std::string> {
		return {
		    "OPENBLAS_NUM_THREADS=1",    "OMP_NUM_THREADS=1",        "HOWDY_COMPARE_VERDICT_FD=3",
		    "HOWDY_COMPARE_CONFIG_FD=4", "HOWDY_COMPARE_MODEL_FD=5",
		};
	}

	// Whether Spawn placed a source above every fixed number at each of fds 3, 4 and 5, in order.
	auto PlacesDescriptors(const PosixSpawnCapture &capture) -> bool {
		const std::array targets{kVerdictFd, kConfigFd, kModelFd};
		if (capture.adddup2.size() != targets.size()) {
			return false;
		}
		for (std::size_t index = 0; index < targets.size(); ++index) {
			const auto &action = capture.adddup2.at(index);
			if (action.new_fd != targets.at(index) || action.fd < kFirstUnusedFd) {
				return false;
			}
		}
		return true;
	}

	auto TestWatchdogTimeoutReapsBlockedChild() -> bool {
		const pid_t child_pid = SpawnBlockedChild();
		if (!Expect(child_pid > 0, "watchdog timeout child spawned")) {
			return false;
		}

		const int status = howdy::test::prompt_coordinator::WaitUntilForTestChild(
		    child_pid, std::chrono::steady_clock::now() + 40ms, nullptr, nullptr, -1);
		return Expect(status == TimeoutWaitStatus(),
		              "watchdog timeout returns synthetic timeout status") &&
		       Expect(ChildReaped(child_pid), "watchdog timeout reaps blocked child");
	}

	auto TestWatchdogKillsSigtermIgnoringChild() -> bool {
		const pid_t child_pid = SpawnSigtermIgnoringChild();
		if (!Expect(child_pid > 0, "SIGTERM-ignoring watchdog child spawned")) {
			return false;
		}

		const int status = howdy::test::prompt_coordinator::WaitUntilForTestChild(
		    child_pid, std::chrono::steady_clock::now() + 40ms, nullptr, nullptr, -1);
		return Expect(status == TimeoutWaitStatus(),
		              "SIGTERM-ignoring child returns synthetic timeout status") &&
		       Expect(ChildReaped(child_pid), "SIGTERM-ignoring child is SIGKILLed and reaped");
	}

	auto TestWatchdogPreservesNaturalExitStatus() -> bool {
		const pid_t child_pid = SpawnChild(17, 10ms);
		if (!Expect(child_pid > 0, "natural watchdog child spawned")) {
			return false;
		}

		const int status = howdy::test::prompt_coordinator::WaitUntilForTestChild(
		    child_pid, std::chrono::steady_clock::now() + 1s, nullptr, nullptr, -1);
		return Expect(status == howdy::native::EncodeWaitExitStatus(17),
		              "watchdog preserves natural exit wait status") &&
		       Expect(ChildReaped(child_pid), "watchdog reaps naturally exited child");
	}

	auto TestWatchdogBlockedSigchldNaturalExit() -> bool {
		ScopedSignalBlock const blocked_sigchld(SIGCHLD);
		if (!Expect(blocked_sigchld.Valid(), "blocked SIGCHLD guard installs")) {
			return false;
		}

		const pid_t child_pid = SpawnChild(17, 10ms);
		if (!Expect(child_pid > 0, "blocked SIGCHLD natural-exit child spawned")) {
			return false;
		}

		const int status = howdy::test::prompt_coordinator::WaitUntilForTestChild(
		    child_pid, std::chrono::steady_clock::now() + 1s, nullptr, nullptr, -1);
		return Expect(status == howdy::native::EncodeWaitExitStatus(17),
		              "blocked SIGCHLD preserves natural compare exit status") &&
		       Expect(ChildReaped(child_pid), "blocked SIGCHLD reaps natural compare child");
	}

	auto TestWatchdogBlockedSigchldTimeout() -> bool {
		ScopedSignalBlock const blocked_sigchld(SIGCHLD);
		if (!Expect(blocked_sigchld.Valid(), "blocked SIGCHLD timeout guard installs")) {
			return false;
		}

		const pid_t child_pid = SpawnBlockedChild();
		if (!Expect(child_pid > 0, "blocked SIGCHLD timeout child spawned")) {
			return false;
		}

		const auto start  = std::chrono::steady_clock::now();
		const int  status = howdy::test::prompt_coordinator::WaitUntilForTestChild(
		    child_pid, start + 40ms, nullptr, nullptr, -1);
		const auto elapsed = std::chrono::steady_clock::now() - start;
		return Expect(status == TimeoutWaitStatus(),
		              "blocked SIGCHLD returns synthetic compare timeout status") &&
		       Expect(elapsed < 2s, "blocked SIGCHLD compare timeout remains bounded") &&
		       Expect(ChildReaped(child_pid), "blocked SIGCHLD reaps timed-out compare child");
	}

	auto TestWatchdogBlockedSigtermTimeout() -> bool {
		ScopedSignalBlock const blocked_sigterm(SIGTERM);
		if (!Expect(blocked_sigterm.Valid(), "blocked SIGTERM timeout guard installs")) {
			return false;
		}

		const pid_t child_pid = SpawnBlockedChild();
		if (!Expect(child_pid > 0, "blocked SIGTERM timeout child spawned")) {
			return false;
		}

		const auto start  = std::chrono::steady_clock::now();
		const int  status = howdy::test::prompt_coordinator::WaitUntilForTestChild(
		    child_pid, start + 40ms, nullptr, nullptr, -1);
		const auto elapsed = std::chrono::steady_clock::now() - start;
		return Expect(status == TimeoutWaitStatus(),
		              "blocked SIGTERM returns synthetic compare timeout status") &&
		       Expect(elapsed >= 40ms, "blocked SIGTERM compare timeout honors deadline") &&
		       Expect(elapsed < 2s, "blocked SIGTERM compare timeout remains bounded") &&
		       Expect(ChildReaped(child_pid),
		              "blocked SIGTERM falls back to SIGKILL and reaps compare child");
	}

	auto TestWaitFailure(bool pidfd_failure, bool bad_event) -> bool {
		const pid_t child = SpawnBlockedChild();
		if (!Expect(child > 0, "failure path spawns child")) {
			return false;
		}
		WaitState().fail_pidfd         = pidfd_failure;
		WaitState().invalid_poll_event = bad_event;
		WaitState().poll_error         = !pidfd_failure && !bad_event ? EIO : 0;
		const int status               = howdy::test::prompt_coordinator::WaitUntilForTestChild(
		    child, std::chrono::steady_clock::now() + 5s, nullptr, nullptr, -1);
		WaitState().fail_pidfd = false;
		return Expect(status ==
		                  howdy::native::EncodeWaitExitStatus(howdy::native::CompareExit::kAbort),
		              "pidfd/ppoll failure returns abort") &&
		       Expect(ChildReaped(child), "pidfd/ppoll failure reaps child");
	}

	// A compare that cannot be signalled but ends within the grace is still reaped.
	auto TestUnsignallableChildIsReapedWithinGrace() -> bool {
		std::array<int, 2> gate{-1, -1};
		if (!Expect(pipe2(gate.data(), O_CLOEXEC) == 0, "unsignallable child gate created")) {
			return false;
		}
		const pid_t child = fork();
		if (child == 0) {
			close(gate.at(1));
			char byte = 0;
			_exit(read(gate.at(0), &byte, 1) == 0 ? 17 : EXIT_FAILURE);
		}
		close(gate.at(0));
		if (!Expect(child > 0, "unsignallable child spawned")) {
			close(gate.at(1));
			return false;
		}
		// The child exits exactly when the grace poll starts, so load cannot outrun the grace.
		WaitState().release_fd        = gate.at(1);
		WaitState().release_pid       = child;
		WaitState().signal_refused    = false;
		WaitState().fail_pidfd_signal = true;
		WaitState().fail_kill         = true;
		const int status              = howdy::test::prompt_coordinator::WaitUntilForTestChild(
		    child, std::chrono::steady_clock::now() + 10ms, nullptr, nullptr, -1);
		WaitState().fail_pidfd_signal = false;
		WaitState().fail_kill         = false;
		if (WaitState().release_fd >= 0) {
			close(std::exchange(WaitState().release_fd, -1));
		}
		return Expect(status ==
		                  howdy::native::EncodeWaitExitStatus(howdy::native::CompareExit::kAbort),
		              "unsignallable compare ends with an abort") &&
		       Expect(ChildReaped(child), "unsignallable compare is reaped within the grace");
	}

	// A kernel or filter that rejects pidfd waits falls back to the PID and still works.
	auto TestUnusablePidfdWaitFallsBackToPid() -> bool {
		const pid_t child = howdy::test::process::SpawnSignaledChild(SIGUSR1);
		if (!Expect(child > 0, "unusable pidfd wait spawns child")) {
			return false;
		}
		WaitState().waitid_error = EINVAL;
		const int status         = howdy::test::prompt_coordinator::WaitUntilForTestChild(
		    child, std::chrono::steady_clock::now() + 5s, nullptr, nullptr, -1);
		WaitState().waitid_error = 0;
		bool ok = Expect(WIFSIGNALED(status) && WTERMSIG(status) == SIGUSR1,
		                 "unusable pidfd wait still reports the real status") &&
		          Expect(ChildReaped(child), "unusable pidfd wait reaps child by PID");

		const pid_t stuck = SpawnSigtermIgnoringChild();
		if (!Expect(stuck > 0, "unusable pidfd wait spawns resistant child")) {
			return false;
		}
		WaitState().waitid_error = EINVAL;
		const int timeout_status = howdy::test::prompt_coordinator::WaitUntilForTestChild(
		    stuck, std::chrono::steady_clock::now(), nullptr, nullptr, -1);
		WaitState().waitid_error = 0;
		ok &= Expect(timeout_status == TimeoutWaitStatus(),
		             "unusable pidfd wait keeps the timeout outcome") &&
		      Expect(ChildReaped(stuck), "unusable pidfd wait kills and reaps resistant child");
		return ok;
	}

	// A child the application already reaped is never signalled: its PID may be reused.
	auto TestReapedElsewhereIsNotSignalled() -> bool {
		const pid_t child = SpawnBlockedChild();
		if (!Expect(child > 0, "reaped-elsewhere spawns child")) {
			return false;
		}
		// Opened before the injection: the spawn-time pidfd exists, and a later pidfd wait is
		// what reports the child gone.
		const howdy::native::ScopedFd pidfd(howdy::pam::runtime::OpenChildPidfd(child));
		WaitState().waitid_error = ECHILD;
		WaitState().term_calls = WaitState().kill_calls = 0;
		const int status                                = howdy::pam::compare_process::WaitUntil(
		    child, pidfd.Get(), -1, std::chrono::steady_clock::now() + 5s, nullptr, nullptr, -1);
		WaitState().waitid_error = 0;
		const bool untouched     = WaitState().term_calls == 0 && WaitState().kill_calls == 0;
		howdy::pam::compare_process::CancelAndReap(child, -1);
		return Expect(status ==
		                  howdy::native::EncodeWaitExitStatus(howdy::native::CompareExit::kAbort),
		              "child reaped elsewhere returns abort") &&
		       Expect(untouched, "child reaped elsewhere is not signalled");
	}

	// A refused pidfd signal is delivered by PID once the child is confirmed unreaped.
	auto TestRefusedPidfdSignalFallsBackToPid() -> bool {
		const pid_t child = SpawnSigtermIgnoringChild();
		if (!Expect(child > 0, "refused pidfd signal spawns child")) {
			return false;
		}
		WaitState().fail_pidfd_signal = true;
		WaitState().term_calls = WaitState().kill_calls = 0;
		const int status = howdy::test::prompt_coordinator::WaitUntilForTestChild(
		    child, std::chrono::steady_clock::now(), nullptr, nullptr, -1);
		WaitState().fail_pidfd_signal = false;
		return Expect(status == TimeoutWaitStatus(), "refused pidfd signal keeps the timeout") &&
		       Expect(WaitState().term_calls == 1 && WaitState().kill_calls == 1,
		              "refused pidfd signals are delivered by PID") &&
		       Expect(ChildReaped(child), "refused pidfd signal still reaps child");
	}

	auto TestEventfdWakesWait() -> bool {
		const howdy::native::ScopedFd cancellation(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK));
		if (!Expect(cancellation.Valid(), "creates cancellation eventfd")) {
			return false;
		}
		const pid_t child = SpawnBlockedChild();
		if (!Expect(child > 0, "eventfd cancellation spawns child")) {
			return false;
		}
		WaitState().poll_entered.store(false);
		std::thread canceller([&] -> void {
			while (!WaitState().poll_entered.load()) {
				std::this_thread::yield();
			}
			(void)eventfd_write(cancellation.Get(), 1);
		});
		const auto  start  = std::chrono::steady_clock::now();
		const int   status = howdy::test::prompt_coordinator::WaitUntilForTestChild(
		    child, start + 5s, nullptr, nullptr, cancellation.Get());
		canceller.join();
		return Expect(status ==
		                  howdy::native::EncodeWaitExitStatus(howdy::native::CompareExit::kAbort),
		              "eventfd wakes blocked wait with abort") &&
		       Expect(std::chrono::steady_clock::now() - start < 1s,
		              "eventfd cancellation wakes before watchdog") &&
		       Expect(ChildReaped(child), "eventfd cancellation reaps child") &&
		       Expect(fcntl(cancellation.Get(), F_GETFD) >= 0,
		              "wait does not close borrowed cancellation fd") &&
		       Expect(fcntl(WaitState().last_pidfd, F_GETFD) < 0 && errno == EBADF,
		              "wait closes owned pidfd");
	}

	auto TestGracePollFailure() -> bool {
		const pid_t child = SpawnSigtermIgnoringChild();
		if (!Expect(child > 0, "grace failure spawns child")) {
			return false;
		}
		WaitState().poll_calls     = 0;
		WaitState().fail_poll_call = 2;
		const int status           = howdy::test::prompt_coordinator::WaitUntilForTestChild(
		    child, std::chrono::steady_clock::now() + 40ms, nullptr, nullptr, -1);
		WaitState().fail_poll_call = 0;
		return Expect(status ==
		                  howdy::native::EncodeWaitExitStatus(howdy::native::CompareExit::kAbort),
		              "grace ppoll failure aborts instead of timeout") &&
		       Expect(ChildReaped(child), "grace ppoll failure reaps child");
	}

	auto TestReadyChildPrecedesCancellation() -> bool {
		const howdy::native::ScopedFd cancellation(eventfd(1, EFD_CLOEXEC | EFD_NONBLOCK));
		if (!Expect(cancellation.Valid(), "creates ready eventfd")) {
			return false;
		}
		const pid_t child = howdy::test::process::SpawnSignaledChild(SIGUSR1);
		if (!Expect(child > 0, "signal status test spawns child")) {
			return false;
		}
		siginfo_t info{};
		while (waitid(P_PID, static_cast<id_t>(child), &info, WEXITED | WNOWAIT) < 0 &&
		       errno == EINTR) {
		}
		const int status = howdy::test::prompt_coordinator::WaitUntilForTestChild(
		    child, std::chrono::steady_clock::now() + 5s, nullptr, nullptr, cancellation.Get());
		return Expect(WIFSIGNALED(status) && WTERMSIG(status) == SIGUSR1 &&
		                  status == WaitState().reaped_status,
		              "ready child preserves exact signal status before cancellation") &&
		       Expect(ChildReaped(child), "ready signalled child is reaped");
	}

	auto TestInterruptedWaitAndEscalation() -> bool {
		const pid_t child = SpawnSigtermIgnoringChild();
		if (!Expect(child > 0, "escalation spawns resistant child")) {
			return false;
		}
		WaitState().poll_error = EINTR;
		WaitState().poll_calls = WaitState().term_calls = WaitState().kill_calls = 0;
		const auto start  = std::chrono::steady_clock::now();
		const int  status = howdy::test::prompt_coordinator::WaitUntilForTestChild(
		    child, start + 40ms, nullptr, nullptr, -1);
		const auto elapsed = std::chrono::steady_clock::now() - start;
		return Expect(status == TimeoutWaitStatus(), "interrupted ppoll preserves timeout") &&
		       Expect(WaitState().poll_calls == 3,
		              "one interrupted wait, one deadline wait, one grace wait") &&
		       Expect(WaitState().term_calls == 1 && WaitState().kill_calls == 1,
		              "timeout sends SIGTERM then SIGKILL") &&
		       Expect(WIFSIGNALED(WaitState().reaped_status) &&
		                  WTERMSIG(WaitState().reaped_status) == SIGKILL,
		              "resistant child dies from SIGKILL") &&
		       Expect(elapsed >= 290ms && elapsed < 2s,
		              "timeout honors deadline and 250ms grace") &&
		       Expect(ChildReaped(child), "escalation reaps child");
	}

	auto TestProductionSpawnAdapter(const howdy::pam::CompareLaunchRequest &request,
	                                const std::vector<std::string>         &expected_argv,
	                                const std::string                      &label,
	                                const std::vector<std::string> &expected_environment) -> bool {
		PosixSpawnCapture capture;
		pid_t             child_pid      = -1;
		int               raw_pidfd      = -1;
		int               raw_verdict_fd = -1;
		const int         result         = howdy::pam::compare_process::Spawn(
		    request, &child_pid, &raw_pidfd, &raw_verdict_fd, PosixSpawnOperations(&capture));
		const howdy::native::ScopedFd pidfd(raw_pidfd);
		const howdy::native::ScopedFd verdict_fd(raw_verdict_fd);
		const bool reaped = child_pid > 0 && waitpid(child_pid, nullptr, 0) == child_pid;
		const bool binds_stdin_null =
		    !capture.addopen.empty() &&
		    capture.addopen.front() == howdy::test::prompt_coordinator::OpenAction{
		                                   .fd    = STDIN_FILENO,
		                                   .path  = "/dev/null",
		                                   .flags = O_RDONLY,
		                               };

		return Expect(result == 0, label + " returns spawn success") &&
		       Expect(pidfd.Get() > STDERR_FILENO, label + " takes a pidfd for the child") &&
		       Expect(verdict_fd.Get() > STDERR_FILENO,
		              label + " returns the verdict socket above stdio") &&
		       Expect(binds_stdin_null && capture.addopen_actions == capture.initialized_actions,
		              label + " redirects stdin to /dev/null on the initialized actions") &&
		       Expect(PlacesDescriptors(capture),
		              label + " places the verdict socket, config and model at fds 3 to 5") &&
		       Expect(reaped, label + " leaves the child for the caller to reap") &&
		       Expect(capture.init_calls == 1, label + " initializes file actions once") &&
		       Expect(capture.addclosefrom_calls == 1, label + " adds close-from action once") &&
		       Expect(capture.closefrom_fd == kFirstUnusedFd,
		              label + " closes descriptors above the model descriptor") &&
		       Expect(capture.spawn_calls == 1, label + " calls posix_spawn once") &&
		       Expect(capture.spawn_actions != nullptr,
		              label + " passes non-null file actions to spawn") &&
		       Expect(capture.spawn_actions == capture.initialized_actions &&
		                  capture.spawn_actions == capture.closefrom_actions,
		              label + " passes initialized close-from actions to spawn") &&
		       Expect(capture.destroy_calls == 1, label + " destroys file actions once") &&
		       Expect(capture.destroyed_actions == capture.initialized_actions,
		              label + " destroys initialized file actions") &&
		       Expect(child_pid == capture.next_pid, label + " preserves spawned PID") &&
		       Expect(capture.path == kCompareProcessPath, label + " preserves executable path") &&
		       Expect(capture.argv == expected_argv, label + " preserves exact argv") &&
		       Expect(capture.environment == expected_environment,
		              label + " preserves exact environment");
	}

	auto TestProductionDescriptorEnvironment() -> bool {
		const CompareRequestFiles files;
		return TestProductionSpawnAdapter(files.Request(), {kCompareProcessPath, "alice"},
		                                  "production descriptor mode",
		                                  DescriptorModeEnvironment());
	}

	auto TestOwnedLaunchRequestFromTemporaries() -> bool {
		const CompareRequestFiles              files;
		const howdy::pam::CompareLaunchRequest request =
		    files.Request(std::string("temporary-user"));
		return TestProductionSpawnAdapter(request, {kCompareProcessPath, "temporary-user"},
		                                  "owned temporary request", DescriptorModeEnvironment());
	}

	// The descriptors are required: a request without them never reaches posix_spawn.
	auto TestProductionRejectsIncompleteRequest() -> bool {
		bool                      ok = true;
		const CompareRequestFiles files;
		for (const auto &[request, label] : {
		         std::pair{howdy::pam::CompareLaunchRequest{
		                       .username  = "alice",
		                       .config_fd = -1,
		                       .model_fd  = files.ModelFd(),
		                   },
				           "request without a config descriptor"},
		         std::pair{howdy::pam::CompareLaunchRequest{
		                       .username  = "alice",
		                       .config_fd = files.ConfigFd(),
		                       .model_fd  = -1,
		                   },
				           "request without a model descriptor"},
		         std::pair{howdy::pam::CompareLaunchRequest{
		                       .username  = "",
		                       .config_fd = files.ConfigFd(),
		                       .model_fd  = files.ModelFd(),
		                   },
				           "request without a username"},
		     }) {
			PosixSpawnCapture capture;
			pid_t             child_pid      = -1;
			int               unused_pidfd   = -1;
			int               unused_verdict = -1;
			const int         result =
			    howdy::pam::compare_process::Spawn(request, &child_pid, &unused_pidfd,
				                                   &unused_verdict, PosixSpawnOperations(&capture));
			ok &= Expect(result == EINVAL && capture.init_calls == 0 && capture.spawn_calls == 0,
			             std::string(label) + " is rejected before spawning");
		}
		return ok;
	}

	auto TestProductionFileActionsInitFailure() -> bool {
		const CompareRequestFiles files;
		PosixSpawnCapture         capture;
		capture.init_result      = ENOMEM;
		pid_t     child_pid      = -1;
		int       unused_pidfd   = -1;
		int       unused_verdict = -1;
		const int result =
		    howdy::pam::compare_process::Spawn(files.Request(), &child_pid, &unused_pidfd,
			                                   &unused_verdict, PosixSpawnOperations(&capture));

		return Expect(result == ENOMEM, "file-actions init failure preserves error") &&
		       Expect(capture.init_calls == 1, "file-actions init failure initializes once") &&
		       Expect(capture.addopen_calls == 0 && capture.adddup2_calls == 0 &&
		                  capture.addclosefrom_calls == 0,
		              "file-actions init failure adds no file actions") &&
		       Expect(unused_verdict == -1,
		              "file-actions init failure returns no verdict socket") &&
		       Expect(capture.spawn_calls == 0, "file-actions init failure does not spawn") &&
		       Expect(capture.destroy_calls == 0,
		              "file-actions init failure does not destroy uninitialized actions") &&
		       Expect(child_pid == -1, "file-actions init failure leaves child PID unchanged");
	}

	auto TestProductionClosefromFailure() -> bool {
		const CompareRequestFiles files;
		PosixSpawnCapture         capture;
		capture.addclosefrom_result = EINVAL;
		pid_t     child_pid         = -1;
		int       unused_pidfd      = -1;
		int       unused_verdict    = -1;
		const int result =
		    howdy::pam::compare_process::Spawn(files.Request(), &child_pid, &unused_pidfd,
			                                   &unused_verdict, PosixSpawnOperations(&capture));

		return Expect(result == EINVAL, "close-from setup failure preserves error") &&
		       Expect(capture.init_calls == 1, "close-from setup failure initializes once") &&
		       Expect(capture.addclosefrom_calls == 1,
		              "close-from setup failure adds action once") &&
		       Expect(PlacesDescriptors(capture) && capture.closefrom_fd == kFirstUnusedFd,
		              "close-from setup failure begins above the model descriptor") &&
		       Expect(unused_verdict == -1, "close-from setup failure returns no verdict socket") &&
		       Expect(capture.spawn_calls == 0, "close-from setup failure does not spawn") &&
		       Expect(capture.destroy_calls == 1,
		              "close-from setup failure destroys initialized actions once") &&
		       Expect(capture.destroyed_actions == capture.initialized_actions,
		              "close-from setup failure destroys initialized file actions") &&
		       Expect(child_pid == -1, "close-from setup failure leaves child PID unchanged");
	}

#ifdef HOWDY_HAVE_PIDFD_SPAWN
	// The production spawn path on a shell that exits at once.
	auto SpawnShellWithPidfd(const howdy::pam::compare_process::SpawnRequest &request) -> int {
		std::string           shell("/bin/sh");
		std::string           flag("-c");
		std::string           script("exit 0");
		std::array<char *, 4> argv{shell.data(), flag.data(), script.data(), nullptr};
		std::array<char *, 1> envp{nullptr};
		return howdy::pam::runtime::SpawnChild(request.child_pid, request.child_pidfd,
		                                       {
		                                           .path    = shell.c_str(),
		                                           .actions = request.actions,
		                                           .argv    = argv.data(),
		                                           .envp    = envp.data(),
		                                       });
	}
#endif

	// Where pidfd_spawn exists, the pidfd comes with compare, so Spawn never opens one: with
	// pidfd_open failing everywhere (SpawnChild's pidfd_getpid probe then proves nothing and
	// pidfd_spawn is still tried), the spawn still succeeds and hands back that pidfd.
	auto TestProductionSpawnTakesPidfdWithChild() -> bool {
#ifdef HOWDY_HAVE_PIDFD_SPAWN
		const CompareRequestFiles files;
		auto                      operations = howdy::pam::compare_process::ProductionOperations();
		operations.spawn                     = SpawnShellWithPidfd;
		pid_t child_pid                      = -1;
		int   raw_pidfd                      = -1;
		int   raw_verdict_fd                 = -1;
		WaitState().fail_pidfd               = true;
		const int result                     = howdy::pam::compare_process::Spawn(
		    files.Request(), &child_pid, &raw_pidfd, &raw_verdict_fd, operations);
		WaitState().fail_pidfd = false;
		const howdy::native::ScopedFd     pidfd(raw_pidfd);
		const howdy::native::ScopedFd     verdict_fd(raw_verdict_fd);
		howdy::pam::runtime::TrackedChild child(child_pid, pidfd.Get());
		int                               status = 0;
		return Expect(result == 0 && child_pid > 0 && pidfd.Get() > STDERR_FILENO &&
		                  verdict_fd.Valid(),
		              "compare's pidfd comes from pidfd_spawn without a pidfd_open") &&
		       Expect(child.Wait(true, &status) == howdy::pam::runtime::ChildState::kReaped &&
		                  WIFEXITED(status) && WEXITSTATUS(status) == 0,
		              "the compare child is reaped through the spawn-time pidfd");
#else
		return true;
#endif
	}

	auto SpawnBlockedForTest(const howdy::pam::compare_process::SpawnRequest &request) -> int {
		const pid_t child_pid = SpawnBlockedChild();
		if (child_pid <= 0) {
			return EAGAIN;
		}
		*request.child_pid = child_pid;
		// Recorded so a test can reap the child even when Spawn reports no child.
		static_cast<PosixSpawnCapture *>(request.context)->next_pid = child_pid;
		return 0;
	}

	// Spawn takes the pidfd before returning, so the wait names the child even if the PAM
	// application reaps it and the PID is reused before the wait starts.
	auto TestProductionSpawnReturnsPidfd() -> bool {
		PosixSpawnCapture capture;
		auto              operations             = PosixSpawnOperations(&capture);
		operations.spawn                         = SpawnBlockedForTest;
		pid_t                     child_pid      = -1;
		int                       raw_pidfd      = -1;
		int                       raw_verdict_fd = -1;
		const CompareRequestFiles files;
		const int                 result = howdy::pam::compare_process::Spawn(
		    files.Request(), &child_pid, &raw_pidfd, &raw_verdict_fd, operations);
		const howdy::native::ScopedFd pidfd(raw_pidfd);
		const howdy::native::ScopedFd verdict_fd(raw_verdict_fd);
		if (!Expect(result == 0 && child_pid > 0, "spawn with pidfd starts the child")) {
			return false;
		}
		const int status = howdy::pam::compare_process::WaitUntil(
		    child_pid, pidfd.Get(), -1, std::chrono::steady_clock::now(), nullptr, nullptr, -1);
		return Expect(pidfd.Get() > STDERR_FILENO, "spawn returns a pidfd above stdio") &&
		       Expect(status == TimeoutWaitStatus(), "wait through the spawn pidfd times out") &&
		       Expect(ChildReaped(child_pid), "wait through the spawn pidfd reaps the child");
	}

	// A child reaped by the application is never waited on by PID afterwards, even where pidfd
	// waits are unavailable: its PID may already name one of the application's other children.
	auto TestReapedElsewhereStopsPidFallback() -> bool {
		const pid_t child_pid = SpawnChild(EXIT_SUCCESS);
		if (!Expect(child_pid > 0, "reaped-elsewhere signal test spawns child")) {
			return false;
		}
		const howdy::native::ScopedFd pidfd(howdy::pam::runtime::OpenChildPidfd(child_pid));
		int                           status = 0;
		bool ok = Expect(pidfd.Valid(), "reaped-elsewhere signal test opens a pidfd");
		ok &= Expect(waitpid(child_pid, &status, 0) == child_pid, "application reaps the child");
		howdy::pam::runtime::TrackedChild child(child_pid, pidfd.Get());
		WaitState().waitid_error = EINVAL;
		const int  signal_result = child.Signal(SIGKILL);
		const auto start         = std::chrono::steady_clock::now();
		const auto wait_state    = child.Wait(true, &status);
		const int  wait_error    = errno;
		const auto elapsed       = std::chrono::steady_clock::now() - start;
		WaitState().waitid_error = 0;
		ok &= Expect(signal_result == ECHILD, "signalling a reaped child reports ECHILD");
		ok &= Expect(wait_state == howdy::pam::runtime::ChildState::kError && wait_error == ECHILD,
		             "a reaped child is not waited on again");
		ok &= Expect(elapsed < 200ms, "the wait after ECHILD does not block on the PID");
		return ok;
	}

	// Without a pidfd at spawn the child is stopped right away and the spawn fails closed,
	// instead of opening a pidfd later from a PID that may have been reused.
	auto TestSpawnFailsClosedWithoutPidfd() -> bool {
		PosixSpawnCapture capture;
		auto              operations = PosixSpawnOperations(&capture);
		operations.spawn             = SpawnBlockedForTest;
		pid_t child_pid              = -1;
		int   raw_pidfd              = -1;
		int   raw_verdict_fd         = -1;
		WaitState().fail_pidfd       = true;
		const CompareRequestFiles files;
		const int                 result = howdy::pam::compare_process::Spawn(
		    files.Request(), &child_pid, &raw_pidfd, &raw_verdict_fd, operations);
		WaitState().fail_pidfd = false;
		const pid_t spawned    = capture.next_pid;
		return Expect(result != 0, "spawn without a pidfd reports failure") &&
		       Expect(child_pid == -1 && raw_pidfd == -1 && raw_verdict_fd == -1,
		              "spawn without a pidfd returns no child") &&
		       Expect(spawned <= 0 || ChildReaped(spawned),
		              "spawn without a pidfd stops and reaps the child");
	}

	// The wait never opens a pidfd from a PID that may have been reused: without the spawn-time
	// pidfd it stops the child by PID and fails closed.
	auto TestWaitWithoutPidfdFailsClosed() -> bool {
		const pid_t child_pid = SpawnBlockedChild();
		if (!Expect(child_pid > 0, "no-pidfd wait spawns child")) {
			return false;
		}
		const auto start   = std::chrono::steady_clock::now();
		const int  status  = howdy::pam::compare_process::WaitUntil(child_pid, -1, -1, start + 5s,
		                                                            nullptr, nullptr, -1);
		const auto elapsed = std::chrono::steady_clock::now() - start;
		return Expect(status ==
		                  howdy::native::EncodeWaitExitStatus(howdy::native::CompareExit::kAbort),
		              "no-pidfd wait returns abort") &&
		       Expect(elapsed < 1s, "no-pidfd wait does not wait for the deadline") &&
		       Expect(ChildReaped(child_pid), "no-pidfd wait stops and reaps the child");
	}

	// OpenChildPidfd refuses a process that is not a child of this one, such as one that took
	// over a reaped child's PID.
	auto TestChildPidfdRefusesNonChild() -> bool {
		errno           = 0;
		const int pidfd = howdy::pam::runtime::OpenChildPidfd(getppid());
		const int error = errno;
		if (pidfd >= 0) {
			(void)close(pidfd);
		}
		return Expect(pidfd < 0 && error == ECHILD, "a pidfd for a non-child is refused");
	}

	// When the pidfd could not be taken because the child is already gone, its PID is left
	// alone; any other failure stops the child by PID.
	auto TestStopChildWithoutPidfd() -> bool {
		const pid_t child_pid = SpawnBlockedChild();
		if (!Expect(child_pid > 0, "stop-without-pidfd test spawns child")) {
			return false;
		}
		int status = 0;
		static_assert(howdy::pam::runtime::PidfdErrorMeansChildGone(EINVAL) &&
		              !howdy::pam::runtime::PidfdErrorMeansChildGone(EMFILE) &&
		              !howdy::pam::runtime::PidfdErrorMeansChildGone(ENOSYS));
		howdy::pam::runtime::StopChildWithoutPidfd(child_pid, ESRCH);
		howdy::pam::runtime::StopChildWithoutPidfd(child_pid, ECHILD);
		howdy::pam::runtime::StopChildWithoutPidfd(child_pid, EINVAL);
		const bool untouched = waitpid(child_pid, &status, WNOHANG) == 0;
		howdy::pam::runtime::StopChildWithoutPidfd(child_pid, ENOSYS);
		return Expect(untouched, "a child reported gone is not signalled by PID") &&
		       Expect(ChildReaped(child_pid), "another pidfd failure stops and reaps the child");
	}

	// A pidfd that lands on stdio and cannot be moved off it because of a low descriptor limit
	// fails with EMFILE, never with the EINVAL that would read as a child already gone.
	auto TestPidfdMoveFailureIsNotChildGone() -> bool {
		const pid_t probe = fork();
		if (probe == 0) {
			const pid_t child = fork();
			if (child == 0) {
				pause();
				_exit(EXIT_SUCCESS);
			}
			// pidfd_open takes the free fd 0, and the move to fd 3 is above the limit.
			(void)close(STDIN_FILENO);
			rlimit limit{};
			(void)getrlimit(RLIMIT_NOFILE, &limit);
			limit.rlim_cur = STDERR_FILENO + 1;
			(void)setrlimit(RLIMIT_NOFILE, &limit);
			errno           = 0;
			const int pidfd = howdy::pam::runtime::OpenChildPidfd(child);
			const int error = errno;
			(void)RealKill(child, SIGKILL);
			(void)waitpid(child, nullptr, 0);
			_exit(child > 0 && pidfd < 0 && error == EMFILE ? EXIT_SUCCESS : EXIT_FAILURE);
		}
		int status = 0;
		return Expect(probe > 0 && waitpid(probe, &status, 0) == probe && WIFEXITED(status) &&
		                  WEXITSTATUS(status) == EXIT_SUCCESS,
		              "a pidfd that cannot leave stdio reports EMFILE, not a child gone");
	}

	// With the pidfd signal refused and no way to confirm the PID, nothing is signalled.
	auto TestUnconfirmedPidIsNotSignalled() -> bool {
		const pid_t child_pid = SpawnBlockedChild();
		if (!Expect(child_pid > 0, "unconfirmed-PID test spawns child")) {
			return false;
		}
		const howdy::native::ScopedFd     pidfd(howdy::pam::runtime::OpenChildPidfd(child_pid));
		howdy::pam::runtime::TrackedChild child(child_pid, pidfd.Get());
		WaitState().fail_pidfd_signal = true;
		WaitState().waitid_any_error  = EPERM;
		WaitState().term_calls = WaitState().kill_calls = 0;
		const int result                                = child.Signal(SIGKILL);
		WaitState().waitid_any_error                    = 0;
		WaitState().fail_pidfd_signal                   = false;
		const bool untouched                            = WaitState().kill_calls == 0;
		(void)howdy::pam::runtime::KillAndReap(child);
		return Expect(result == EPERM, "an unconfirmed PID reports the peek failure") &&
		       Expect(untouched, "an unconfirmed PID is not signalled") &&
		       Expect(ChildReaped(child_pid), "unconfirmed-PID test reaps the child afterwards");
	}

	// On Linux 5.3 (waitid(P_PIDFD) returns EINVAL), PeekTracked falls back to P_PID in both
	// SignalByPid and AwaitExit even before Wait() has been called, while pid <= 0 never invokes
	// waitid(P_PID, pid).
	auto TestPeekTrackedLinux53EinvalFallback() -> bool {
		bool ok = true;

		// 1. SignalByPid falls back to P_PID on EINVAL before Wait() runs.
		const pid_t blocked = SpawnBlockedChild();
		if (!Expect(blocked > 0, "Linux 5.3 signal-peek test spawns child")) {
			return false;
		}
		const howdy::native::ScopedFd blocked_pidfd(howdy::pam::runtime::OpenChildPidfd(blocked));
		howdy::pam::runtime::TrackedChild blocked_child(blocked, blocked_pidfd.Get());
		WaitState().waitid_error      = EINVAL;
		WaitState().fail_pidfd_signal = true;
		WaitState().kill_calls        = 0;
		const int signal_result       = blocked_child.Signal(SIGKILL);
		WaitState().fail_pidfd_signal = false;
		WaitState().waitid_error      = 0;
		int       status              = 0;
		const int reaped =
		    blocked_child.Wait(true, &status) == howdy::pam::runtime::ChildState::kReaped ? 1 : 0;
		ok &= Expect(signal_result == 0 && WaitState().kill_calls == 1,
		             "SignalByPid falls back to P_PID when waitid(P_PIDFD) returns EINVAL");
		ok &= Expect(reaped == 1 && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
		             "child signalled via Linux 5.3 EINVAL fallback is reaped with SIGKILL");

		// 2. Non-EINVAL waitid(P_PIDFD) failure does NOT fall back to P_PID.
		const pid_t filtered = SpawnBlockedChild();
		if (!Expect(filtered > 0, "filtered P_PIDFD peek test spawns child")) {
			return false;
		}
		const howdy::native::ScopedFd filtered_pidfd(howdy::pam::runtime::OpenChildPidfd(filtered));
		howdy::pam::runtime::TrackedChild filtered_child(filtered, filtered_pidfd.Get());
		WaitState().waitid_error      = EPERM;
		WaitState().fail_pidfd_signal = true;
		WaitState().kill_calls        = 0;
		const int filtered_signal     = filtered_child.Signal(SIGKILL);
		WaitState().fail_pidfd_signal = false;
		WaitState().waitid_error      = 0;
		ok &= Expect(filtered_signal == EPERM && WaitState().kill_calls == 0,
		             "non-EINVAL waitid(P_PIDFD) error refuses PID fallback in SignalByPid");
		(void)howdy::pam::runtime::KillAndReap(filtered_child);

		// 3. AwaitExit falls back to P_PID on EINVAL before Wait(), and pid <= 0 fails closed.
		const pid_t exited = SpawnChild(EXIT_SUCCESS);
		if (!Expect(exited > 0, "Linux 5.3 await-exit test spawns child")) {
			return false;
		}
		siginfo_t ready_info{};
		(void)RealWaitid(P_PID, static_cast<id_t>(exited), &ready_info,
		                 howdy::native::CombineBitsAs<int>(WEXITED, WNOWAIT));
		// Closed fd forces poll() -> POLLNVAL -> poll_pidfd = false -> PeekTracked with
		// pidfd_wait_usable_ still true.
		const int stale_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
		if (!Expect(stale_fd >= 0 && close(stale_fd) == 0, "prepare closed fd for AwaitExit")) {
			(void)waitpid(exited, nullptr, 0);
			return false;
		}
		howdy::pam::runtime::TrackedChild exited_child(exited, stale_fd);
		WaitState().waitid_error = EINVAL;
		const auto start         = std::chrono::steady_clock::now();
		const bool saw_exit      = exited_child.AwaitExit(500ms);
		const auto elapsed       = std::chrono::steady_clock::now() - start;

		howdy::pam::runtime::TrackedChild unknown_pid_child(-1, stale_fd);
		const bool                        unknown_exited = unknown_pid_child.AwaitExit(500ms);
		WaitState().waitid_error                         = 0;
		ok &= Expect(saw_exit && elapsed < 200ms,
		             "AwaitExit falls back to P_PID on Linux 5.3 EINVAL before Wait()");
		ok &=
		    Expect(waitpid(exited, nullptr, 0) == exited, "Linux 5.3 await-exit test reaps child");
		ok &= Expect(!unknown_exited && unknown_pid_child.WasReapedElsewhere(),
		             "AwaitExit with pid <= 0 marks reaped elsewhere on EINVAL without waitid(-1)");
		return ok;
	}

	// Without a usable PID (pid <= 0), a non-ESRCH pidfd_send_signal failure returns that error
	// without marking the child reaped elsewhere or calling kill(), so pidfd Wait() still works.
	auto TestSignalWithoutPidPreservesNonEsrchError() -> bool {
		const pid_t child_pid = SpawnBlockedChild();
		if (!Expect(child_pid > 0, "signal-without-pid test spawns child")) {
			return false;
		}
		const howdy::native::ScopedFd pidfd(howdy::pam::runtime::OpenChildPidfd(child_pid));
		if (!Expect(pidfd.Valid(), "signal-without-pid test opens child pidfd")) {
			(void)kill(child_pid, SIGKILL);
			(void)waitpid(child_pid, nullptr, 0);
			return false;
		}

		struct SignalCase {
			pid_t pid;
			bool  use_pidfd;
			int   injected_error;
			int   expected_error;
			bool  expected_reaped;
		};

		constexpr auto cases = std::to_array<SignalCase>({
		    {
		        .pid             = -1,
		        .use_pidfd       = true,
		        .injected_error  = EPERM,
		        .expected_error  = EPERM,
		        .expected_reaped = false,
		    },
		    {
		        .pid             = 0,
		        .use_pidfd       = true,
		        .injected_error  = ENOSYS,
		        .expected_error  = ENOSYS,
		        .expected_reaped = false,
		    },
		    {
		        .pid             = -1,
		        .use_pidfd       = true,
		        .injected_error  = ESRCH,
		        .expected_error  = ECHILD,
		        .expected_reaped = true,
		    },
		    {
		        .pid             = 0,
		        .use_pidfd       = true,
		        .injected_error  = ESRCH,
		        .expected_error  = ECHILD,
		        .expected_reaped = true,
		    },
		    {
		        .pid             = -1,
		        .use_pidfd       = false,
		        .injected_error  = 0,
		        .expected_error  = ECHILD,
		        .expected_reaped = true,
		    },
		    {
		        .pid             = 0,
		        .use_pidfd       = false,
		        .injected_error  = 0,
		        .expected_error  = ECHILD,
		        .expected_reaped = true,
		    },
		});

		bool ok = true;
		for (const auto &test_case : cases) {
			howdy::pam::runtime::TrackedChild child(test_case.pid,
			                                        test_case.use_pidfd ? pidfd.Get() : -1);
			WaitState().pidfd_signal_error = test_case.injected_error;
			WaitState().kill_calls         = 0;
			const int result               = child.Signal(SIGKILL);
			WaitState().pidfd_signal_error = 0;
			ok &= Expect(result == test_case.expected_error &&
			                 child.WasReapedElsewhere() == test_case.expected_reaped &&
			                 WaitState().kill_calls == 0,
			             "Signal with pid <= 0 classifies pidfd error without calling kill");
			if (!test_case.expected_reaped) {
				int status = 0;
				ok &=
				    Expect(child.Wait(false, &status) == howdy::pam::runtime::ChildState::kRunning,
					       "non-ESRCH pidfd signal failure leaves pidfd child waitable");
			}
		}

		howdy::pam::runtime::TrackedChild cleanup(-1, pidfd.Get());
		const auto                        reaped = howdy::pam::runtime::KillAndReap(cleanup);
		ok &= Expect(reaped.has_value() && WIFSIGNALED(*reaped) && WTERMSIG(*reaped) == SIGKILL,
		             "signal-without-pid test stops and reaps child through pidfd");
		ok &= Expect(ChildReaped(child_pid), "signal-without-pid child is reaped");
		return ok;
	}

	// On Linux 5.3 (waitid(P_PIDFD) returns EINVAL), OpenChildPidfd falls back to P_PID to accept
	// our own child and still reject a non-child with ECHILD.
	auto TestOpenChildPidfdLinux53EinvalFallback() -> bool {
		bool        ok        = true;
		const pid_t child_pid = SpawnBlockedChild();
		if (!Expect(child_pid > 0, "Linux 5.3 OpenChildPidfd test spawns child")) {
			return false;
		}
		WaitState().waitid_error = EINVAL;
		const howdy::native::ScopedFd child_pidfd(howdy::pam::runtime::OpenChildPidfd(child_pid));
		errno                    = 0;
		const int self_pidfd     = howdy::pam::runtime::OpenChildPidfd(getpid());
		const int self_error     = errno;
		WaitState().waitid_error = 0;

		ok &= Expect(child_pidfd.Get() > STDERR_FILENO,
		             "OpenChildPidfd accepts our child via P_PID fallback on Linux 5.3 EINVAL");
		ok &= Expect(self_pidfd < 0 && self_error == ECHILD,
		             "OpenChildPidfd rejects non-child with ECHILD via P_PID fallback on EINVAL");
		howdy::pam::runtime::TrackedChild child(child_pid, child_pidfd.Get());
		(void)howdy::pam::runtime::KillAndReap(child);
		ok &= Expect(ChildReaped(child_pid), "Linux 5.3 OpenChildPidfd child is reaped");
		return ok;
	}

	// Without a usable PID (pid <= 0), Wait() and AwaitExit() fail closed with ECHILD / false and
	// never call waitpid() or waitid(P_PID, <=0).
	auto TestTrackedChildWithoutPidFailsClosed() -> bool {
		const pid_t child_pid = SpawnBlockedChild();
		if (!Expect(child_pid > 0, "tracked-child-without-pid test spawns child")) {
			return false;
		}
		const howdy::native::ScopedFd pidfd(howdy::pam::runtime::OpenChildPidfd(child_pid));
		if (!Expect(pidfd.Valid(), "tracked-child-without-pid test opens pidfd")) {
			(void)kill(child_pid, SIGKILL);
			(void)waitpid(child_pid, nullptr, 0);
			return false;
		}

		bool ok = true;
		for (const pid_t bad_pid : {pid_t{-1}, pid_t{0}}) {
			// 1. Wait with pid <= 0 and no pidfd fails closed without waitpid().
			howdy::pam::runtime::TrackedChild no_pidfd(bad_pid, -1);
			int                               status = 0;
			WaitState().waitpid_calls                = 0;
			errno                                    = 0;
			const auto wait_no_pidfd                 = no_pidfd.Wait(false, &status);
			ok &= Expect(wait_no_pidfd == howdy::pam::runtime::ChildState::kError &&
			                 errno == ECHILD && no_pidfd.WasReapedElsewhere() &&
			                 WaitState().waitpid_calls == 0,
			             "Wait with pid <= 0 and no pidfd fails closed without waitpid");

			// 2. Wait with pid <= 0 and pidfd on Linux 5.3 EINVAL fails closed without waitpid().
			howdy::pam::runtime::TrackedChild einval_pidfd(bad_pid, pidfd.Get());
			WaitState().waitid_error  = EINVAL;
			WaitState().waitpid_calls = 0;
			errno                     = 0;
			const auto wait_einval    = einval_pidfd.Wait(false, &status);
			WaitState().waitid_error  = 0;
			ok &=
			    Expect(wait_einval == howdy::pam::runtime::ChildState::kError && errno == ECHILD &&
				           einval_pidfd.WasReapedElsewhere() && WaitState().waitpid_calls == 0,
				       "Wait with pid <= 0 on Linux 5.3 EINVAL fails closed without waitpid");

			// 3. AwaitExit with pid <= 0 and no pidfd fails closed without waitid().
			howdy::pam::runtime::TrackedChild await_no_pidfd(bad_pid, -1);
			WaitState().waitid_calls = 0;
			const bool exited        = await_no_pidfd.AwaitExit(500ms);
			ok &= Expect(!exited && await_no_pidfd.WasReapedElsewhere() &&
			                 WaitState().waitid_calls == 0,
			             "AwaitExit with pid <= 0 and no pidfd fails closed without waitid");
		}

		howdy::pam::runtime::TrackedChild cleanup(-1, pidfd.Get());
		(void)howdy::pam::runtime::KillAndReap(cleanup);
		ok &= Expect(ChildReaped(child_pid), "tracked-child-without-pid child is reaped");
		return ok;
	}

	// A pidfd wait refused for any reason but a missing P_PIDFD fails closed instead of moving to
	// a waitpid that could reap another child which reused the PID.
	auto TestFilteredPidfdWaitDoesNotUsePid() -> bool {
		const pid_t child_pid = SpawnBlockedChild();
		if (!Expect(child_pid > 0, "filtered pidfd wait spawns child")) {
			return false;
		}
		WaitState().waitid_error = EPERM;
		const int status         = howdy::test::prompt_coordinator::WaitUntilForTestChild(
		    child_pid, std::chrono::steady_clock::now() + 5s, nullptr, nullptr, -1);
		WaitState().waitid_error = 0;
		// The wait stops compare through its pidfd but leaves the reap to us: a blocking waitpid
		// here still finds it.
		int        probe      = 0;
		const bool still_ours = waitpid(child_pid, &probe, 0) == child_pid;
		return Expect(status ==
		                  howdy::native::EncodeWaitExitStatus(howdy::native::CompareExit::kAbort),
		              "a filtered pidfd wait returns abort") &&
		       Expect(still_ours, "a filtered pidfd wait does not reap by PID") &&
		       Expect(WIFSIGNALED(probe) && WTERMSIG(probe) == SIGKILL,
		              "a filtered pidfd wait still stops the child");
	}

	// A PAM application may run with stdio closed; the pidfd must not take its place.
	auto TestChildPidfdAvoidsClosedStdio() -> bool {
		const pid_t child_pid = SpawnBlockedChild();
		if (!Expect(child_pid > 0, "stdio pidfd test spawns child")) {
			return false;
		}
		const howdy::native::ScopedFd saved_stdin(fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, 3));
		bool                          ok = Expect(saved_stdin.Valid(), "saves stdin");
		ok &= Expect(close(STDIN_FILENO) == 0, "closes stdin to free descriptor 0");
		const howdy::native::ScopedFd pidfd(howdy::pam::runtime::OpenChildPidfd(child_pid));
		ok &= Expect(dup2(saved_stdin.Get(), STDIN_FILENO) == STDIN_FILENO, "restores stdin");
		ok &= Expect(pidfd.Get() > STDERR_FILENO, "pidfd is placed above stdio");
		const int status = howdy::pam::compare_process::WaitUntil(
		    child_pid, pidfd.Get(), -1, std::chrono::steady_clock::now(), nullptr, nullptr, -1);
		ok &= Expect(status == TimeoutWaitStatus() && ChildReaped(child_pid),
		             "stdio pidfd test reaps the child");
		return ok;
	}

	// The verdict socket is required in descriptor mode: when it cannot be set up or passed,
	// compare is not started.
	auto ExpectSpawnFailsWithoutVerdictSocket(PosixSpawnCapture &capture, int expected_error,
	                                          const std::string &label) -> bool {
		const CompareRequestFiles files;
		pid_t                     child_pid      = -1;
		int                       raw_pidfd      = -1;
		int                       raw_verdict_fd = -1;
		const int                 result =
		    howdy::pam::compare_process::Spawn(files.Request(), &child_pid, &raw_pidfd,
			                                   &raw_verdict_fd, PosixSpawnOperations(&capture));
		return Expect(result == expected_error, label + " fails the spawn") &&
		       Expect(capture.spawn_calls == 0 && child_pid == -1, label + " starts no compare") &&
		       Expect(raw_pidfd == -1 && raw_verdict_fd == -1,
		              label + " returns no pidfd or verdict socket");
	}

	auto TestProductionSpawnWithoutVerdictSocket() -> bool {
		PosixSpawnCapture unopened;
		unopened.socketpair_error = EMFILE;
		PosixSpawnCapture unpassed;
		unpassed.adddup2_fail_index = 0;
		unpassed.adddup2_result     = ENOMEM;
		PosixSpawnCapture no_config;
		no_config.adddup2_fail_index = 1;
		no_config.adddup2_result     = ENOMEM;
		PosixSpawnCapture no_model;
		no_model.adddup2_fail_index = 2;
		no_model.adddup2_result     = EBADF;
		return ExpectSpawnFailsWithoutVerdictSocket(unopened, EMFILE,
		                                            "a verdict socket that cannot be opened") &&
		       Expect(unopened.init_calls == 0,
		              "an unopened verdict socket adds no file actions") &&
		       ExpectSpawnFailsWithoutVerdictSocket(unpassed, ENOMEM,
		                                            "a verdict socket that cannot be passed") &&
		       Expect(unpassed.destroy_calls == 1,
		              "an unpassed verdict socket destroys the file actions") &&
		       ExpectSpawnFailsWithoutVerdictSocket(no_config, ENOMEM,
		                                            "a config descriptor that cannot be passed") &&
		       ExpectSpawnFailsWithoutVerdictSocket(no_model, EBADF,
		                                            "a model descriptor that cannot be passed");
	}

	auto TestProductionSpawnFailure() -> bool {
		const CompareRequestFiles files;
		PosixSpawnCapture         capture;
		capture.spawn_result     = EACCES;
		pid_t     child_pid      = -1;
		int       unused_pidfd   = -1;
		int       unused_verdict = -1;
		const int result =
		    howdy::pam::compare_process::Spawn(files.Request(), &child_pid, &unused_pidfd,
			                                   &unused_verdict, PosixSpawnOperations(&capture));

		return Expect(result == EACCES, "production spawn failure preserves error") &&
		       Expect(capture.init_calls == 1, "production spawn failure initializes once") &&
		       Expect(capture.addclosefrom_calls == 1,
		              "production spawn failure adds close-from action once") &&
		       Expect(capture.spawn_calls == 1,
		              "production spawn failure calls posix_spawn once") &&
		       Expect(capture.spawn_actions != nullptr,
		              "production spawn failure passes non-null file actions") &&
		       Expect(capture.destroy_calls == 1,
		              "production spawn failure destroys file actions once") &&
		       Expect(child_pid == -1, "production spawn failure leaves child PID unchanged") &&
		       Expect(capture.path == kCompareProcessPath,
		              "production spawn failure preserves executable path") &&
		       Expect(capture.argv == std::vector<std::string>{kCompareProcessPath, "alice"},
		              "production spawn failure preserves exact argv") &&
		       Expect(unused_verdict == -1, "production spawn failure returns no verdict socket") &&
		       Expect(capture.environment == DescriptorModeEnvironment(),
		              "production spawn failure preserves the fixed descriptor-mode environment");
	}

	auto TestProductionSpawnRejectsNullCallbacks() -> bool {
		const CompareRequestFiles                     files;
		const howdy::pam::compare_process::Operations empty_operations{};
		pid_t                                         child_pid      = 42;
		int                                           raw_pidfd      = -1;
		int                                           raw_verdict_fd = -1;
		const int                                     result = howdy::pam::compare_process::Spawn(
		    files.Request(), &child_pid, &raw_pidfd, &raw_verdict_fd, empty_operations);
		PosixSpawnCapture capture;
		auto              missing_addopen    = PosixSpawnOperations(&capture);
		missing_addopen.file_actions_addopen = nullptr;
		int       addopen_pidfd              = -1;
		int       addopen_verdict            = -1;
		const int missing_addopen_result     = howdy::pam::compare_process::Spawn(
		    files.Request(), &child_pid, &addopen_pidfd, &addopen_verdict, missing_addopen);
		return Expect(result == EINVAL, "spawn rejects missing operations callbacks") &&
		       Expect(raw_pidfd == -1 && raw_verdict_fd == -1,
		              "rejected spawn leaves pidfd and verdict descriptors unset") &&
		       Expect(missing_addopen_result == EINVAL && addopen_pidfd == -1 &&
		                  addopen_verdict == -1 && capture.init_calls == 0,
		              "spawn rejects null file_actions_addopen callback before setup");
	}

	enum class HostStdioState : std::uint8_t {
		kInherited,
		kCloseOnExec,
		kClosed,
	};

	struct StdioActionCase {
		const char    *label;
		HostStdioState stdin_state;
		HostStdioState stdout_state;
		HostStdioState stderr_state;
		bool           expect_stdout_null;
		bool           expect_stderr_null;
	};

	auto ConfigureHostStdioFd(int fd, HostStdioState state) -> bool {
		if (state == HostStdioState::kClosed) {
			return close(fd) == 0 || errno == EBADF;
		}
		const int open_flags = fd == STDIN_FILENO ? O_RDONLY : O_WRONLY;
		// Keep the temporary fd strictly above STDERR_FILENO so dup2 never aliases `fd` and
		// ~ScopedFd never closes the target stdio slot we just configured.
		const auto owned = howdy::test::prompt_coordinator::OpenNullAboveStdio(
		    howdy::native::CombineBitsAs<int>(open_flags, O_CLOEXEC));
		if (!owned.Valid() || dup2(owned.Get(), fd) != fd) {
			return false;
		}
		if (state == HostStdioState::kCloseOnExec) {
			return fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
		}
		return fcntl(fd, F_SETFD, 0) == 0;
	}

	auto RunStdioActionCaseInSubprocess(const StdioActionCase &test_case) -> bool {
		using howdy::test::prompt_coordinator::OpenAction;
		const pid_t probe = fork();
		if (probe == 0) {
			const CompareRequestFiles files;
			if (!ConfigureHostStdioFd(STDIN_FILENO, test_case.stdin_state) ||
			    !ConfigureHostStdioFd(STDOUT_FILENO, test_case.stdout_state) ||
			    !ConfigureHostStdioFd(STDERR_FILENO, test_case.stderr_state)) {
				_exit(EXIT_FAILURE);
			}
			PosixSpawnCapture capture;
			pid_t             child_pid      = -1;
			int               raw_pidfd      = -1;
			int               raw_verdict_fd = -1;
			const int         result =
			    howdy::pam::compare_process::Spawn(files.Request(), &child_pid, &raw_pidfd,
				                                   &raw_verdict_fd, PosixSpawnOperations(&capture));
			const howdy::native::ScopedFd pidfd(raw_pidfd);
			const howdy::native::ScopedFd verdict_fd(raw_verdict_fd);
			const bool reaped = child_pid > 0 && waitpid(child_pid, nullptr, 0) == child_pid;

			std::vector<OpenAction> expected_opens = {
			    {.fd = STDIN_FILENO, .path = "/dev/null", .flags = O_RDONLY},
			};
			if (test_case.expect_stdout_null) {
				expected_opens.push_back(
				    {.fd = STDOUT_FILENO, .path = "/dev/null", .flags = O_WRONLY});
			}
			if (test_case.expect_stderr_null) {
				expected_opens.push_back(
				    {.fd = STDERR_FILENO, .path = "/dev/null", .flags = O_WRONLY});
			}
			const bool ok = result == 0 && reaped && pidfd.Get() > STDERR_FILENO &&
			                verdict_fd.Get() > STDERR_FILENO && capture.addopen == expected_opens &&
			                PlacesDescriptors(capture) && capture.closefrom_fd == kFirstUnusedFd;
			_exit(ok ? EXIT_SUCCESS : EXIT_FAILURE);
		}
		int status = 0;
		return Expect(probe > 0 && waitpid(probe, &status, 0) == probe && WIFEXITED(status) &&
		                  WEXITSTATUS(status) == EXIT_SUCCESS,
		              test_case.label);
	}

	auto TestProductionSpawnStdioFileActions() -> bool {
		const auto cases = std::to_array<StdioActionCase>({
		    {
		        .label              = "inherited stdio redirects only stdin to /dev/null",
		        .stdin_state        = HostStdioState::kInherited,
		        .stdout_state       = HostStdioState::kInherited,
		        .stderr_state       = HostStdioState::kInherited,
		        .expect_stdout_null = false,
		        .expect_stderr_null = false,
		    },
		    {
		        .label              = "closed stdin and stdout redirect fds 0 and 1 to /dev/null",
		        .stdin_state        = HostStdioState::kClosed,
		        .stdout_state       = HostStdioState::kClosed,
		        .stderr_state       = HostStdioState::kInherited,
		        .expect_stdout_null = true,
		        .expect_stderr_null = false,
		    },
		    {
		        .label              = "CLOEXEC stdout and closed stderr redirect all stdio fds",
		        .stdin_state        = HostStdioState::kInherited,
		        .stdout_state       = HostStdioState::kCloseOnExec,
		        .stderr_state       = HostStdioState::kClosed,
		        .expect_stdout_null = true,
		        .expect_stderr_null = true,
		    },
		    {
		        .label              = "all closed stdio fds redirect fds 0, 1, and 2 to /dev/null",
		        .stdin_state        = HostStdioState::kClosed,
		        .stdout_state       = HostStdioState::kClosed,
		        .stderr_state       = HostStdioState::kClosed,
		        .expect_stdout_null = true,
		        .expect_stderr_null = true,
		    },
		    {
		        .label              = "CLOEXEC stderr redirects fds 0 and 2 while keeping stdout",
		        .stdin_state        = HostStdioState::kCloseOnExec,
		        .stdout_state       = HostStdioState::kInherited,
		        .stderr_state       = HostStdioState::kCloseOnExec,
		        .expect_stdout_null = false,
		        .expect_stderr_null = true,
		    },
		});
		bool       ok    = true;
		for (const auto &test_case : cases) {
			ok &= RunStdioActionCaseInSubprocess(test_case);
		}
		return ok;
	}

	auto TestProductionAddopenFailure() -> bool {
		const pid_t probe = fork();
		if (probe == 0) {
			const CompareRequestFiles files;
			if (!ConfigureHostStdioFd(STDOUT_FILENO, HostStdioState::kClosed) ||
			    !ConfigureHostStdioFd(STDERR_FILENO, HostStdioState::kCloseOnExec)) {
				_exit(EXIT_FAILURE);
			}
			for (const int fail_index : {0, 1, 2}) {
				PosixSpawnCapture capture;
				capture.addopen_fail_index = fail_index;
				capture.addopen_result     = ENOMEM;
				pid_t     child_pid        = -1;
				int       raw_pidfd        = -1;
				int       raw_verdict_fd   = -1;
				const int result           = howdy::pam::compare_process::Spawn(
				    files.Request(), &child_pid, &raw_pidfd, &raw_verdict_fd,
				    PosixSpawnOperations(&capture));
				const bool ok = result == ENOMEM && child_pid == -1 && raw_pidfd == -1 &&
				                raw_verdict_fd == -1 && capture.spawn_calls == 0 &&
				                capture.adddup2_calls == 0 && capture.destroy_calls == 1 &&
				                capture.addopen_calls == fail_index + 1;
				if (!ok) {
					_exit(EXIT_FAILURE);
				}
			}
			_exit(EXIT_SUCCESS);
		}
		int status = 0;
		return Expect(probe > 0 && waitpid(probe, &status, 0) == probe && WIFEXITED(status) &&
		                  WEXITSTATUS(status) == EXIT_SUCCESS,
		              "addopen failure for stdin, stdout, or stderr aborts spawn and cleans up");
	}

	constexpr auto kStdioProbeInheritedStdoutMode = "--stdio-probe-inherited-stdout";
	constexpr auto kStdioProbeAllNullMode         = "--stdio-probe-all-null";

	auto DescriptorIsNullDevice(int fd) -> bool {
		struct stat fd_status{};
		struct stat null_status{};
		if (fstat(fd, &fd_status) != 0 || stat("/dev/null", &null_status) != 0) {
			return false;
		}
		return S_ISCHR(fd_status.st_mode) && fd_status.st_dev == null_status.st_dev &&
		       fd_status.st_ino == null_status.st_ino && fd_status.st_rdev == null_status.st_rdev;
	}

	auto RunStdioProbeMode(std::string_view mode) -> int {
		if (!DescriptorIsNullDevice(STDIN_FILENO) || !DescriptorIsNullDevice(STDERR_FILENO)) {
			return EXIT_FAILURE;
		}
		char byte = '\0';
		if (read(STDIN_FILENO, &byte, 1) != 0) {
			return EXIT_FAILURE;
		}
		if (mode == kStdioProbeInheritedStdoutMode) {
			constexpr std::string_view marker = "ok";
			return std::cmp_equal(write(STDOUT_FILENO, marker.data(), marker.size()), marker.size())
			           ? EXIT_SUCCESS
					   : EXIT_FAILURE;
		}
		if (mode == kStdioProbeAllNullMode) {
			return DescriptorIsNullDevice(STDOUT_FILENO) ? EXIT_SUCCESS : EXIT_FAILURE;
		}
		return EXIT_FAILURE;
	}

	auto TryRunStdioProbeFromArgs(int argc, char **argv) -> std::optional<int> {
		if (argc != 2 || argv == nullptr) {
			return std::nullopt;
		}
		const auto arguments = std::span(argv, static_cast<std::size_t>(argc));
		if (arguments.back() == nullptr) {
			return std::nullopt;
		}
		const std::string_view arg(arguments.back());
		if (arg == kStdioProbeInheritedStdoutMode || arg == kStdioProbeAllNullMode) {
			return RunStdioProbeMode(arg);
		}
		return std::nullopt;
	}

	auto SpawnSelfStdioProbe(const howdy::pam::compare_process::SpawnRequest &request) -> int {
		const auto           &mode_arg = *static_cast<const std::string *>(request.context);
		std::string           self_path("/proc/self/exe");
		std::string           mode(mode_arg);
		std::array<char *, 3> argv{self_path.data(), mode.data(), nullptr};
		std::array<char *, 1> envp{nullptr};
		return howdy::pam::runtime::SpawnChild(request.child_pid, request.child_pidfd,
		                                       {
		                                           .path    = self_path.c_str(),
		                                           .actions = request.actions,
		                                           .argv    = argv.data(),
		                                           .envp    = envp.data(),
		                                       });
	}

	auto RunRealChildStdioProbe(std::string mode_arg) -> bool {
		const CompareRequestFiles files;
		auto                      operations = howdy::pam::compare_process::ProductionOperations();
		operations.context                   = &mode_arg;
		operations.spawn                     = SpawnSelfStdioProbe;
		pid_t     child_pid                  = -1;
		int       raw_pidfd                  = -1;
		int       raw_verdict_fd             = -1;
		const int result                     = howdy::pam::compare_process::Spawn(
		    files.Request(), &child_pid, &raw_pidfd, &raw_verdict_fd, operations);
		const howdy::native::ScopedFd pidfd(raw_pidfd);
		const howdy::native::ScopedFd verdict_fd(raw_verdict_fd);
		if (result != 0 || child_pid <= 0 || !pidfd.Valid() || !verdict_fd.Valid()) {
			return false;
		}
		howdy::pam::runtime::TrackedChild child(child_pid, pidfd.Get());
		int                               status = 0;
		return child.Wait(true, &status) == howdy::pam::runtime::ChildState::kReaped &&
		       WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS;
	}

	auto OpenPipeAboveStdio(howdy::native::ScopedFd &read_end, howdy::native::ScopedFd &write_end)
	    -> bool {
		std::array<int, 2> fds{-1, -1};
		if (pipe2(fds.data(), O_CLOEXEC) != 0) {
			return false;
		}
		read_end.Reset(fds.at(0));
		write_end.Reset(fds.at(1));
		return howdy::test::prompt_coordinator::MoveFdAboveStdio(read_end) &&
		       howdy::test::prompt_coordinator::MoveFdAboveStdio(write_end);
	}

	auto TestRealChildIsolatesStdinAndClosedStderr() -> bool {
		// Host stdin carries secret input; stdout is an inherited pipe; stderr is closed.
		const pid_t stdin_probe = fork();
		if (stdin_probe == 0) {
			howdy::native::ScopedFd stdin_read;
			howdy::native::ScopedFd stdin_write;
			howdy::native::ScopedFd stdout_read;
			howdy::native::ScopedFd stdout_write;
			if (!OpenPipeAboveStdio(stdin_read, stdin_write) ||
			    !OpenPipeAboveStdio(stdout_read, stdout_write)) {
				_exit(EXIT_FAILURE);
			}
			constexpr std::string_view secret = "secret-password\n";
			if (!std::cmp_equal(write(stdin_write.Get(), secret.data(), secret.size()),
			                    secret.size())) {
				_exit(EXIT_FAILURE);
			}
			// Close the write end before spawning so even a regressed child reading stdin gets EOF
			// instead of deadlocking the test.
			stdin_write.Reset();
			if (dup2(stdin_read.Get(), STDIN_FILENO) != STDIN_FILENO ||
			    dup2(stdout_write.Get(), STDOUT_FILENO) != STDOUT_FILENO ||
			    !ConfigureHostStdioFd(STDERR_FILENO, HostStdioState::kClosed) ||
			    fcntl(STDERR_FILENO, F_GETFD) >= 0 || errno != EBADF) {
				_exit(EXIT_FAILURE);
			}
			const bool child_ok = RunRealChildStdioProbe(kStdioProbeInheritedStdoutMode);
			// Close both local stdout write ends before reading stdout_read so a child failure
			// yields immediate EOF instead of blocking read().
			stdout_write.Reset();
			(void)close(STDOUT_FILENO);
			std::array<char, 32> captured_stdout{};
			const ssize_t        out_bytes =
			    read(stdout_read.Get(), captured_stdout.data(), captured_stdout.size());
			std::array<char, 32> remaining_stdin{};
			const ssize_t        in_bytes =
			    read(STDIN_FILENO, remaining_stdin.data(), remaining_stdin.size());
			const bool stdout_ok =
			    out_bytes == 2 && std::string_view(captured_stdout.data(), 2) == "ok";
			const bool secret_intact =
			    std::cmp_equal(in_bytes, secret.size()) &&
			    std::string_view(remaining_stdin.data(), secret.size()) == secret;
			_exit(child_ok && stdout_ok && secret_intact ? EXIT_SUCCESS : EXIT_FAILURE);
		}
		int stdin_status = 0;
		return Expect(stdin_probe > 0 && waitpid(stdin_probe, &stdin_status, 0) == stdin_probe &&
		                  WIFEXITED(stdin_status) && WEXITSTATUS(stdin_status) == EXIT_SUCCESS,
		              "real spawned child receives /dev/null on stdin and closed stderr while "
		              "keeping inherited stdout");
	}

	auto TestRealChildReplacesCloexecStdoutAndClosedStderr() -> bool {
		// Host stdout is FD_CLOEXEC and stderr is closed; all stdio fds become /dev/null.
		const pid_t cloexec_probe = fork();
		if (cloexec_probe == 0) {
			if (!ConfigureHostStdioFd(STDOUT_FILENO, HostStdioState::kCloseOnExec) ||
			    !ConfigureHostStdioFd(STDERR_FILENO, HostStdioState::kClosed) ||
			    fcntl(STDERR_FILENO, F_GETFD) >= 0 || errno != EBADF) {
				_exit(EXIT_FAILURE);
			}
			_exit(RunRealChildStdioProbe(kStdioProbeAllNullMode) ? EXIT_SUCCESS : EXIT_FAILURE);
		}
		int cloexec_status = 0;
		return Expect(cloexec_probe > 0 &&
		                  waitpid(cloexec_probe, &cloexec_status, 0) == cloexec_probe &&
		                  WIFEXITED(cloexec_status) && WEXITSTATUS(cloexec_status) == EXIT_SUCCESS,
		              "real spawned child binds FD_CLOEXEC stdout and closed stderr to /dev/null");
	}

	auto TestProductionSpawnIsolatesChildStdio() -> bool {
		return TestRealChildIsolatesStdinAndClosedStderr() &&
		       TestRealChildReplacesCloexecStdoutAndClosedStderr();
	}

	auto TestProductionSpawnFailsClosedWithoutPidOrPidfd() -> bool {
		const CompareRequestFiles files;
		PosixSpawnCapture         capture;
		auto                      operations = PosixSpawnOperations(&capture);
		operations.spawn = [](const howdy::pam::compare_process::SpawnRequest &) -> int {
			return 0;
		};
		pid_t     child_pid      = 4242;
		int       raw_pidfd      = -1;
		int       raw_verdict_fd = -1;
		const int result         = howdy::pam::compare_process::Spawn(
		    files.Request(), &child_pid, &raw_pidfd, &raw_verdict_fd, operations);
		return Expect(result == ECHILD,
		              "spawn returning success without child pid or pidfd fails closed with "
		              "ECHILD") &&
		       Expect(child_pid == -1 && raw_pidfd == -1 && raw_verdict_fd == -1,
		              "fail-closed spawn clears child pid, pidfd, and verdict socket");
	}

}  // namespace

auto main(int argc, char **argv) -> int {
	if (const auto probe_status = TryRunStdioProbeFromArgs(argc, argv); probe_status.has_value()) {
		return *probe_status;
	}
	bool ok = true;
	ok &= TestGracePollFailure();
	ok &= TestReadyChildPrecedesCancellation();
	ok &= TestWaitFailure(true, false);
	ok &= TestWaitFailure(false, false);
	ok &= TestWaitFailure(false, true);
	ok &= TestEventfdWakesWait();
	ok &= TestInterruptedWaitAndEscalation();
	ok &= TestUnusablePidfdWaitFallsBackToPid();
	ok &= TestReapedElsewhereIsNotSignalled();
	ok &= TestRefusedPidfdSignalFallsBackToPid();
	ok &= TestUnsignallableChildIsReapedWithinGrace();
	ok &= TestWatchdogTimeoutReapsBlockedChild();
	ok &= TestWatchdogKillsSigtermIgnoringChild();
	ok &= TestWatchdogPreservesNaturalExitStatus();
	ok &= TestWatchdogBlockedSigchldNaturalExit();
	ok &= TestWatchdogBlockedSigchldTimeout();
	ok &= TestWatchdogBlockedSigtermTimeout();
	ok &= TestProductionDescriptorEnvironment();
	ok &= TestProductionRejectsIncompleteRequest();
	ok &= TestOwnedLaunchRequestFromTemporaries();
	ok &= TestProductionFileActionsInitFailure();
	ok &= TestProductionClosefromFailure();
	ok &= TestProductionSpawnFailure();
	ok &= TestProductionSpawnRejectsNullCallbacks();
	ok &= TestProductionSpawnStdioFileActions();
	ok &= TestProductionAddopenFailure();
	ok &= TestProductionSpawnIsolatesChildStdio();
	ok &= TestProductionSpawnFailsClosedWithoutPidOrPidfd();
	ok &= TestProductionSpawnWithoutVerdictSocket();
	ok &= TestProductionSpawnReturnsPidfd();
	ok &= TestProductionSpawnTakesPidfdWithChild();
	ok &= TestChildPidfdAvoidsClosedStdio();
	ok &= TestReapedElsewhereStopsPidFallback();
	ok &= TestSpawnFailsClosedWithoutPidfd();
	ok &= TestWaitWithoutPidfdFailsClosed();
	ok &= TestChildPidfdRefusesNonChild();
	ok &= TestStopChildWithoutPidfd();
	ok &= TestPidfdMoveFailureIsNotChildGone();
	ok &= TestUnconfirmedPidIsNotSignalled();
	ok &= TestPeekTrackedLinux53EinvalFallback();
	ok &= TestSignalWithoutPidPreservesNonEsrchError();
	ok &= TestOpenChildPidfdLinux53EinvalFallback();
	ok &= TestTrackedChildWithoutPidFailsClosed();
	ok &= TestFilteredPidfdWaitDoesNotUsePid();
	return ok ? 0 : 1;
}
