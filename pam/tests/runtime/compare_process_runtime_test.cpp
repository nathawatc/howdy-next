#include "paths.hpp"
#include "protocol/compare_descriptors.hpp"
#include "protocol/compare_exit.hpp"
#include "protocol/compare_verdict.hpp"
#include "runtime/child_process.hpp"
#include "runtime/compare_launch.hpp"
#include "runtime/compare_process.hpp"
#include "runtime/compare_process_test_support.hpp"
#include "support/process_status.hpp"
#include "support/process_test_support.hpp"
#include "support/scoped_fd.hpp"
#include "test_support.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <string>
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
#include <sys/types.h>
#include <sys/wait.h>

namespace {
	struct WaitTestState {
		bool              fail_pidfd         = false;
		int               waitid_error       = 0;
		int               waitid_any_error   = 0;  // every waitid, by PID as well as by pidfd
		bool              fail_pidfd_signal  = false;
		int               last_pidfd         = -1;
		int               poll_error         = 0;
		bool              invalid_poll_event = false;
		std::atomic<bool> poll_entered{false};
		int               poll_calls     = 0;
		int               fail_poll_call = 0;
		int               term_calls     = 0;
		int               kill_calls     = 0;
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
	CountSignal(signal);
	return RealKill(pid, signal);
}

auto TestPidfdSendSignal(int pidfd, int signal, siginfo_t *info, unsigned int flags) -> int {
	if (WaitState().fail_pidfd_signal) {
		errno = EPERM;
		return -1;
	}
	CountSignal(signal);
	return RealPidfdSendSignal(pidfd, signal, info, flags);
}

// Records what a pidfd reap consumed, in wait(2) form, ignoring peeks and empty polls.
auto TestWaitid(idtype_t id_type, id_t id, siginfo_t *info, int options) -> int {
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

		return Expect(result == 0, label + " returns spawn success") &&
		       Expect(pidfd.Get() > STDERR_FILENO, label + " takes a pidfd for the child") &&
		       Expect(verdict_fd.Get() > STDERR_FILENO,
		              label + " returns the verdict socket above stdio") &&
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
		       Expect(capture.adddup2_calls == 0 && capture.addclosefrom_calls == 0,
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

}  // namespace

auto main() -> int {
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
	ok &= TestFilteredPidfdWaitDoesNotUsePid();
	return ok ? 0 : 1;
}
