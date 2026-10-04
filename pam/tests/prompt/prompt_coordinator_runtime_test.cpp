#include "paths.hpp"
#include "prompt/prompt_coordinator.hpp"
#include "prompt/prompt_coordinator_fake.hpp"
#include "prompt/workaround.hpp"
#include "protocol/compare_exit.hpp"
#include "runtime/compare_launch.hpp"
#include "runtime/compare_process.hpp"
#include "runtime/compare_process_test_support.hpp"
#include "support/process_status.hpp"
#include "support/process_test_support.hpp"
#include "support/scoped_fd.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <sys/eventfd.h>
#include <sys/poll.h>
#include <sys/types.h>
#include <sys/wait.h>

namespace {
	struct WaitTestState {
		bool              fail_pidfd         = false;
		bool              fail_eventfd       = false;
		bool              fail_eventfd_write = false;
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
extern "C" auto TestEventfd(unsigned int value, int flags) -> int __asm__("__wrap_eventfd");
extern "C" auto RealEventfd(unsigned int value, int flags) -> int __asm__("__real_eventfd");
extern "C" auto TestEventfdWrite(int fd, eventfd_t value) -> int __asm__("__wrap_eventfd_write");
extern "C" auto RealEventfdWrite(int fd, eventfd_t value) -> int __asm__("__real_eventfd_write");
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

auto TestPidfdOpen(pid_t pid, unsigned int flags) -> int {
	if (WaitState().fail_pidfd) {
		errno = ENOSYS;
		return -1;
	}
	WaitState().last_pidfd = RealPidfdOpen(pid, flags);
	return WaitState().last_pidfd;
}

auto TestEventfd(unsigned int value, int flags) -> int {
	if (WaitState().fail_eventfd) {
		errno = EMFILE;
		return -1;
	}
	return RealEventfd(value, flags);
}

auto TestEventfdWrite(int fd, eventfd_t value) -> int {
	if (WaitState().fail_eventfd_write) {
		errno = EIO;
		return -1;
	}
	return RealEventfdWrite(fd, value);
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

auto TestKill(pid_t pid, int signal) -> int {
	if (signal == SIGTERM) {
		++WaitState().term_calls;
	}
	if (signal == SIGKILL) {
		++WaitState().kill_calls;
	}
	return RealKill(pid, signal);
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
	using howdy::pam::PromptCoordinator;
	using howdy::pam::PromptCoordinatorDecision;
	using howdy::pam::Workaround;
	using howdy::test::process::ChildReaped;
	using howdy::test::process::ScopedSignalBlock;
	using howdy::test::process::SpawnBlockedChild;
	using howdy::test::process::SpawnSigtermIgnoringChild;
	using howdy::test::prompt_coordinator::FakeContext;
	using howdy::test::prompt_coordinator::MakeCompareRequest;
	using howdy::test::prompt_coordinator::PosixSpawnCapture;
	using howdy::test::prompt_coordinator::Timeout;
	using howdy::test::prompt_coordinator::TimeoutWaitStatus;

	using howdy::test::Expect;

	auto WatchdogWaitForCompare(void *context, pid_t child_pid,
	                            std::chrono::steady_clock::time_point      deadline,
	                            void                                      *cancellation_context,
	                            howdy::pam::CompareCancellationRequestedFn cancellation_requested,
	                            int                                        cancellation_fd) -> int {
		auto &fake = *static_cast<FakeContext *>(context);
		++fake.wait_calls;
		fake.waited_pid      = child_pid;
		const auto remaining = std::max(deadline - std::chrono::steady_clock::now(),
		                                std::chrono::steady_clock::duration::zero());
		const int  status    = howdy::pam::compare_process::WaitUntil(
		    child_pid, std::chrono::steady_clock::now() + remaining, cancellation_context,
		    cancellation_requested, cancellation_fd);
		if (cancellation_requested != nullptr && cancellation_requested(cancellation_context)) {
			++fake.terminate_calls;
			fake.terminated_pid = child_pid;
		}
		fake.last_wait_status = status;
		return status;
	}

	auto TestWatchdogTimeoutReapsBlockedChild() -> bool {
		const pid_t child_pid = SpawnBlockedChild();
		if (!Expect(child_pid > 0, "watchdog timeout child spawned")) {
			return false;
		}

		const int status = howdy::pam::compare_process::WaitUntil(
		    child_pid, std::chrono::steady_clock::now() + 40ms);
		return Expect(status == TimeoutWaitStatus(),
		              "watchdog timeout returns synthetic timeout status") &&
		       Expect(ChildReaped(child_pid), "watchdog timeout reaps blocked child");
	}

	auto TestWatchdogKillsSigtermIgnoringChild() -> bool {
		const pid_t child_pid = SpawnSigtermIgnoringChild();
		if (!Expect(child_pid > 0, "SIGTERM-ignoring watchdog child spawned")) {
			return false;
		}

		const int status = howdy::pam::compare_process::WaitUntil(
		    child_pid, std::chrono::steady_clock::now() + 40ms);
		return Expect(status == TimeoutWaitStatus(),
		              "SIGTERM-ignoring child returns synthetic timeout status") &&
		       Expect(ChildReaped(child_pid), "SIGTERM-ignoring child is SIGKILLed and reaped");
	}

	auto TestWatchdogPreservesNaturalExitStatus() -> bool {
		const pid_t child_pid = SpawnChild(17, 10ms);
		if (!Expect(child_pid > 0, "natural watchdog child spawned")) {
			return false;
		}

		const int status = howdy::pam::compare_process::WaitUntil(
		    child_pid, std::chrono::steady_clock::now() + 1s);
		return Expect(status == (howdy::native::EncodeWaitExitStatus(17)),
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

		const int status = howdy::pam::compare_process::WaitUntil(
		    child_pid, std::chrono::steady_clock::now() + 1s);
		return Expect(status == (howdy::native::EncodeWaitExitStatus(17)),
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

		const auto start   = std::chrono::steady_clock::now();
		const int  status  = howdy::pam::compare_process::WaitUntil(child_pid, start + 40ms);
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

		const auto start   = std::chrono::steady_clock::now();
		const int  status  = howdy::pam::compare_process::WaitUntil(child_pid, start + 40ms);
		const auto elapsed = std::chrono::steady_clock::now() - start;
		return Expect(status == TimeoutWaitStatus(),
		              "blocked SIGTERM returns synthetic compare timeout status") &&
		       Expect(elapsed >= 40ms, "blocked SIGTERM compare timeout honors deadline") &&
		       Expect(elapsed < 2s, "blocked SIGTERM compare timeout remains bounded") &&
		       Expect(ChildReaped(child_pid),
		              "blocked SIGTERM falls back to SIGKILL and reaps compare child");
	}

	auto TestWatchdogTimeoutKeepsPasswordFallback() -> bool {
		FakeContext context{
		    .token_delay  = 100ms,
		    .token_result = PAM_SUCCESS,
		};
		const pid_t child_pid = SpawnBlockedChild();
		if (!Expect(child_pid > 0, "watchdog fallback child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;
		const auto ops         = Operations(&context, WatchdogWaitForCompare);

		PromptCoordinator coordinator(nullptr, Workaround::kInput, true, false, ops, Timeout(40ms));
		const auto        result = coordinator.Run(MakeCompareRequest());
		return Expect(result.decision == PromptCoordinatorDecision::kPasswordFallback,
		              "watchdog timeout keeps password fallback") &&
		       Expect(result.compare_status == TimeoutWaitStatus(),
		              "password fallback preserves watchdog timeout status") &&
		       Expect(result.pam_status == PAM_SUCCESS,
		              "password fallback preserves PAM success") &&
		       Expect(ChildReaped(child_pid), "password fallback reaps watchdog child");
	}

	auto TestPamSuccessReapsBeforeWatchdog() -> bool {
		FakeContext context;
		const pid_t child_pid = SpawnBlockedChild();
		if (!Expect(child_pid > 0, "PAM-before-watchdog child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;
		const auto ops         = Operations(&context, WatchdogWaitForCompare);

		PromptCoordinator coordinator(nullptr, Workaround::kInput, true, false, ops, Timeout(1s));
		const auto        result = coordinator.Run(MakeCompareRequest());
		return Expect(result.decision == PromptCoordinatorDecision::kPamResult,
		              "PAM success wins before watchdog") &&
		       Expect(context.terminate_calls == 1, "PAM success terminates compare child") &&
		       Expect(context.last_wait_status != TimeoutWaitStatus(),
		              "PAM success does not use watchdog timeout status") &&
		       Expect(ChildReaped(child_pid), "PAM success reaps compare child");
	}

	auto TestEventfdSetupFailure() -> bool {
		FakeContext       context;
		PromptCoordinator coordinator(nullptr, Workaround::kOff, false, false,
		                              Operations(&context, WatchdogWaitForCompare), Timeout(1s));
		WaitState().fail_eventfd = true;
		const auto result        = coordinator.Run(MakeCompareRequest());
		WaitState().fail_eventfd = false;
		return Expect(result.decision == PromptCoordinatorDecision::kPamResult &&
		                  result.pam_status == PAM_SYSTEM_ERR,
		              "eventfd failure fails closed") &&
		       Expect(context.spawn_calls == 0, "eventfd failure never spawns compare");
	}

	auto TestEventfdWriteFailure() -> bool {
		FakeContext context;
		const pid_t child = SpawnBlockedChild();
		if (!Expect(child > 0, "eventfd write failure spawns child")) {
			return false;
		}
		context.next_child_pid = child;
		PromptCoordinator coordinator(nullptr, Workaround::kInput, true, false,
		                              Operations(&context, WatchdogWaitForCompare), Timeout(40ms));
		WaitState().fail_eventfd_write = true;
		const auto result              = coordinator.Run(MakeCompareRequest());
		WaitState().fail_eventfd_write = false;
		return Expect(result.decision == PromptCoordinatorDecision::kPamResult &&
		                  result.pam_status == PAM_SYSTEM_ERR,
		              "eventfd write failure overrides PAM success") &&
		       Expect(ChildReaped(child), "eventfd write failure leaves no child");
	}

	auto TestWaitFailure(bool pidfd_failure, bool bad_event) -> bool {
		const pid_t child = SpawnBlockedChild();
		if (!Expect(child > 0, "failure path spawns child")) {
			return false;
		}
		WaitState().fail_pidfd         = pidfd_failure;
		WaitState().invalid_poll_event = bad_event;
		WaitState().poll_error         = !pidfd_failure && !bad_event ? EIO : 0;
		const int status =
		    howdy::pam::compare_process::WaitUntil(child, std::chrono::steady_clock::now() + 5s);
		WaitState().fail_pidfd = false;
		return Expect(status ==
		                  howdy::native::EncodeWaitExitStatus(howdy::native::CompareExit::kAbort),
		              "pidfd/ppoll failure returns abort") &&
		       Expect(ChildReaped(child), "pidfd/ppoll failure reaps child");
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
		const int   status = howdy::pam::compare_process::WaitUntil(child, start + 5s, nullptr,
		                                                            nullptr, cancellation.Get());
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
		const int status =
		    howdy::pam::compare_process::WaitUntil(child, std::chrono::steady_clock::now() + 40ms);
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
		const int status = howdy::pam::compare_process::WaitUntil(
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
		const auto start   = std::chrono::steady_clock::now();
		const int  status  = howdy::pam::compare_process::WaitUntil(child, start + 40ms);
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
		pid_t             child_pid = -1;
		const int         result =
		    howdy::pam::compare_process::Spawn(request, &child_pid, PosixSpawnOperations(&capture));

		return Expect(result == 0, label + " returns spawn success") &&
		       Expect(capture.init_calls == 1, label + " initializes file actions once") &&
		       Expect(capture.addclosefrom_calls == 1, label + " adds close-from action once") &&
		       Expect(capture.closefrom_fd == STDERR_FILENO + 1,
		              label + " closes descriptors beginning at 3") &&
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

	auto TestProductionDirectRuntimeEnvironment() -> bool {
		return TestProductionSpawnAdapter(
		    MakeCompareRequest("/etc/howdy/config.ini", "alice", "/etc/howdy/models", false),
		    {kCompareProcessPath, "--config", "/etc/howdy/config.ini", "alice"},
		    "production direct runtime", {});
	}

	auto TestProductionStagedRuntimeEnvironment() -> bool {
		return TestProductionSpawnAdapter(
		    MakeCompareRequest("/run/howdy/runtime/config.ini", "alice",
			                   "/run/howdy/runtime/models", true),
		    {kCompareProcessPath, "--config", "/run/howdy/runtime/config.ini", "alice"},
		    "production staged runtime", {"HOWDY_USER_MODELS_DIR=/run/howdy/runtime/models"});
	}

	auto TestOwnedLaunchRequestFromTemporaries() -> bool {
		const howdy::pam::CompareLaunchRequest request = {
		    .config_path     = std::string("/run/howdy/temporary/config.ini"),
		    .username        = std::string("temporary-user"),
		    .user_models_dir = std::string("/run/howdy/temporary/models"),
		    .staged_runtime  = true,
		};

		return TestProductionSpawnAdapter(
		    request,
		    {kCompareProcessPath, "--config", "/run/howdy/temporary/config.ini", "temporary-user"},
		    "owned temporary request", {"HOWDY_USER_MODELS_DIR=/run/howdy/temporary/models"});
	}

	auto TestProductionFileActionsInitFailure() -> bool {
		PosixSpawnCapture capture;
		capture.init_result = ENOMEM;
		pid_t     child_pid = -1;
		const int result    = howdy::pam::compare_process::Spawn(MakeCompareRequest(), &child_pid,
		                                                         PosixSpawnOperations(&capture));

		return Expect(result == ENOMEM, "file-actions init failure preserves error") &&
		       Expect(capture.init_calls == 1, "file-actions init failure initializes once") &&
		       Expect(capture.addclosefrom_calls == 0,
		              "file-actions init failure does not add close-from action") &&
		       Expect(capture.spawn_calls == 0, "file-actions init failure does not spawn") &&
		       Expect(capture.destroy_calls == 0,
		              "file-actions init failure does not destroy uninitialized actions") &&
		       Expect(child_pid == -1, "file-actions init failure leaves child PID unchanged");
	}

	auto TestProductionClosefromFailure() -> bool {
		PosixSpawnCapture capture;
		capture.addclosefrom_result = EINVAL;
		pid_t     child_pid         = -1;
		const int result = howdy::pam::compare_process::Spawn(MakeCompareRequest(), &child_pid,
		                                                      PosixSpawnOperations(&capture));

		return Expect(result == EINVAL, "close-from setup failure preserves error") &&
		       Expect(capture.init_calls == 1, "close-from setup failure initializes once") &&
		       Expect(capture.addclosefrom_calls == 1,
		              "close-from setup failure adds action once") &&
		       Expect(capture.closefrom_fd == STDERR_FILENO + 1,
		              "close-from setup failure begins at descriptor 3") &&
		       Expect(capture.spawn_calls == 0, "close-from setup failure does not spawn") &&
		       Expect(capture.destroy_calls == 1,
		              "close-from setup failure destroys initialized actions once") &&
		       Expect(capture.destroyed_actions == capture.initialized_actions,
		              "close-from setup failure destroys initialized file actions") &&
		       Expect(child_pid == -1, "close-from setup failure leaves child PID unchanged");
	}

	auto TestProductionSpawnFailure() -> bool {
		PosixSpawnCapture capture;
		capture.spawn_result = EACCES;
		pid_t     child_pid  = -1;
		const int result     = howdy::pam::compare_process::Spawn(MakeCompareRequest(), &child_pid,
		                                                          PosixSpawnOperations(&capture));

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
		       Expect(capture.argv == std::vector<std::string>{kCompareProcessPath, "--config",
		                                                       "/etc/howdy/config.ini", "alice"},
		              "production spawn failure preserves exact argv") &&
		       Expect(capture.environment.empty(),
		              "production spawn failure preserves empty direct environment");
	}

}  // namespace

auto main() -> int {
	bool ok = true;
	ok &= TestEventfdSetupFailure();
	ok &= TestEventfdWriteFailure();
	ok &= TestGracePollFailure();
	ok &= TestReadyChildPrecedesCancellation();
	ok &= TestWaitFailure(true, false);
	ok &= TestWaitFailure(false, false);
	ok &= TestWaitFailure(false, true);
	ok &= TestEventfdWakesWait();
	ok &= TestInterruptedWaitAndEscalation();
	ok &= TestWatchdogTimeoutReapsBlockedChild();
	ok &= TestWatchdogKillsSigtermIgnoringChild();
	ok &= TestWatchdogPreservesNaturalExitStatus();
	ok &= TestWatchdogBlockedSigchldNaturalExit();
	ok &= TestWatchdogBlockedSigchldTimeout();
	ok &= TestWatchdogBlockedSigtermTimeout();
	ok &= TestWatchdogTimeoutKeepsPasswordFallback();
	ok &= TestPamSuccessReapsBeforeWatchdog();
	ok &= TestProductionDirectRuntimeEnvironment();
	ok &= TestProductionStagedRuntimeEnvironment();
	ok &= TestOwnedLaunchRequestFromTemporaries();
	ok &= TestProductionFileActionsInitFailure();
	ok &= TestProductionClosefromFailure();
	ok &= TestProductionSpawnFailure();
	return ok ? 0 : 1;
}
