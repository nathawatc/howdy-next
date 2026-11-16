#include "../../src/runtime/auth_helper_process/internal.hpp"
#include "module/auth_flow_test_groups.hpp"
#include "protocol/compare_exit.hpp"
#include "runtime/child_process.hpp"
#include "runtime/compare_process.hpp"
#include "runtime/compare_process_test_support.hpp"
#include "support/eintr.hpp"
#include "support/process_status.hpp"
#include "support/process_test_support.hpp"
#include "support/scoped_fd.hpp"
#include "test_support.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <string>

#include <fcntl.h>
#include <signal.h>  // IWYU pragma: keep
#include <stdlib.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/wait.h>

namespace {
	using howdy::test::Expect;
	using howdy::test::ScopedFd;
	using howdy::test::process::ChildReaped;
	using howdy::test::process::SpawnBlockedChild;
	using howdy::test::process::SpawnExitingChild;
	using howdy::test::process::SpawnSigtermIgnoringChild;

	auto AlwaysCancelCompare(void *context) -> bool {
		(void)context;
		return true;
	}

	struct TerminalBoundaryChild {
		pid_t child_pid              = -1;
		int   release_fd             = -1;
		bool  cancellation_requested = false;
		bool  child_exit_observed    = false;
	};

	auto SpawnTerminalBoundaryChild(int *release_fd) -> pid_t {
		std::array<int, 2> release_pipe = {-1, -1};
		if (pipe2(release_pipe.data(), O_CLOEXEC) != 0) {
			return -1;
		}

		const pid_t child_pid = fork();
		if (child_pid < 0) {
			(void)close(release_pipe.at(0));
			(void)close(release_pipe.at(1));
			return -1;
		}
		if (child_pid == 0) {
			(void)close(release_pipe.at(1));
			char          release     = '\0';
			const ssize_t read_result = howdy::native::RetryOnEintr([&] -> ssize_t {
				return read(release_pipe.at(0), &release, sizeof(release));
			});
			(void)close(release_pipe.at(0));
			_exit(read_result == 1 && release == '1' ? EXIT_SUCCESS : EXIT_FAILURE);
		}

		(void)close(release_pipe.at(0));
		*release_fd = release_pipe.at(1);
		return child_pid;
	}

	auto WaitForChildExit(pid_t child_pid) -> bool {
		siginfo_t info{};
		while (waitid(P_PID, static_cast<id_t>(child_pid), &info, WEXITED | WNOWAIT) < 0) {
			if (errno != EINTR) {
				return false;
			}
		}
		return info.si_pid == child_pid && info.si_code == CLD_EXITED &&
		       info.si_status == EXIT_SUCCESS;
	}

	auto ReleaseTerminalBoundaryChild(void *context) -> bool {
		auto         &boundary     = *static_cast<TerminalBoundaryChild *>(context);
		const char    release      = '1';
		const ssize_t write_result = howdy::native::RetryOnEintr([&] -> ssize_t {
			return write(boundary.release_fd, &release, sizeof(release));
		});
		if (write_result != 1) {
			return boundary.cancellation_requested;
		}

		boundary.child_exit_observed = WaitForChildExit(boundary.child_pid);
		return boundary.cancellation_requested;
	}

	auto ExpectTerminalOutcomePreserved(bool cancellation) -> bool {
		const std::string outcome = cancellation ? "cancellation" : "timeout";
		bool              ok      = true;

		int         release_pipe_fd = -1;
		const pid_t child_pid       = SpawnTerminalBoundaryChild(&release_pipe_fd);
		ScopedFd    release_fd(release_pipe_fd);
		ok &= Expect(child_pid > 0, outcome + " boundary spawns child");
		if (child_pid <= 0) {
			return false;
		}

		TerminalBoundaryChild boundary{
		    .child_pid              = child_pid,
		    .release_fd             = release_fd.Get(),
		    .cancellation_requested = cancellation,
		};
		const auto deadline = cancellation
		                          ? std::chrono::steady_clock::now() + std::chrono::seconds(1)
		                          : std::chrono::steady_clock::now();
		const int  status   = howdy::test::prompt_coordinator::WaitUntilForTestChild(
		    child_pid, deadline, &boundary, ReleaseTerminalBoundaryChild, -1);
		const auto expected_exit   = cancellation ? howdy::native::CompareExit::kAbort
		                                          : howdy::native::CompareExit::kTimeoutReached;
		const int  expected_status = howdy::native::EncodeWaitExitStatus(expected_exit);
		ok &= Expect(boundary.child_exit_observed,
		             outcome + " boundary observes child exit before cleanup wait");
		ok &= Expect(WIFEXITED(status) && status == expected_status,
		             outcome + " preserves terminal outcome after child exit");
		ok &= Expect(ChildReaped(child_pid), outcome + " boundary reaps child");
		return ok;
	}

	// A PAM application that reaps children itself can take the compare child first. The wait
	// must then fail closed without signalling: the PID no longer belongs to compare.
	auto ExpectReapedElsewhereFailsClosed() -> bool {
		bool        ok        = true;
		const pid_t child_pid = SpawnExitingChild(0);
		ok &= Expect(child_pid > 0, "spawns child that the application reaps");
		if (child_pid <= 0) {
			return false;
		}
		int status = 0;
		ok &= Expect(howdy::native::RetryOnEintr([&] -> pid_t {
			             return waitpid(child_pid, &status, 0);
		             }) == child_pid,
		             "application reaps the child first");
		const int result = howdy::test::prompt_coordinator::WaitUntilForTestChild(
		    child_pid, std::chrono::steady_clock::now() + std::chrono::seconds(1), nullptr, nullptr,
		    -1);
		const auto abort_code = static_cast<int>(howdy::native::CompareExit::kAbort);
		ok &= Expect(WIFEXITED(result) && WEXITSTATUS(result) == abort_code,
		             "wait on a child reaped elsewhere returns abort, not its success status");
		return ok;
	}

	// The helper paths share the same rule: a helper the application reaped is an error, and the
	// terminate path reaps one that is still running.
	auto ExpectHelperTrackedThroughPidfd() -> bool {
		using howdy::pam::auth_helper_process::internal::HelperWaitResult;
		using howdy::pam::auth_helper_process::internal::TerminateAndReapHelperProcess;
		using howdy::pam::auth_helper_process::internal::WaitForHelperProcessUntil;
		const auto abort_code = static_cast<int>(howdy::native::CompareExit::kAbort);
		bool       ok         = true;

		const pid_t reaped_child = SpawnExitingChild(0);
		ok &= Expect(reaped_child > 0, "spawns helper that the application reaps");
		if (reaped_child > 0) {
			int status = 0;
			ok &= Expect(howdy::native::RetryOnEintr([&] -> pid_t {
				             return waitpid(reaped_child, &status, 0);
			             }) == reaped_child,
			             "application reaps the helper first");
			const int terminated = TerminateAndReapHelperProcess(reaped_child, -1);
			ok &= Expect(WIFEXITED(terminated) && WEXITSTATUS(terminated) == abort_code,
			             "terminating a helper reaped elsewhere returns abort");
			int        wait_status = 0;
			const auto waited      = WaitForHelperProcessUntil(
			    reaped_child, std::chrono::steady_clock::now() + std::chrono::seconds(1),
			    &wait_status, -1);
			ok &= Expect(waited == HelperWaitResult::kWaitError,
			             "waiting on a helper reaped elsewhere is an error, not success");
		}

		const pid_t stuck_child = SpawnSigtermIgnoringChild();
		ok &= Expect(stuck_child > 0, "spawns SIGTERM-resistant helper");
		if (stuck_child > 0) {
			int        status = 0;
			const auto waited = WaitForHelperProcessUntil(
			    stuck_child, std::chrono::steady_clock::now(), &status, -1);
			ok &= Expect(waited == HelperWaitResult::kTimedOut, "helper deadline reports timeout");
			ok &= Expect(ChildReaped(stuck_child), "helper deadline kills and reaps the helper");
		}

		// The grace wait returns as soon as the helper exits, even with a stale EINTR in errno
		// from an earlier, retried call.
		const pid_t polite_child = SpawnBlockedChild();
		ok &= Expect(polite_child > 0, "spawns helper that exits on SIGTERM");
		if (polite_child > 0) {
			const howdy::native::ScopedFd pidfd(howdy::pam::runtime::OpenChildPidfd(polite_child));
			errno              = EINTR;
			const auto start   = std::chrono::steady_clock::now();
			const int  status  = TerminateAndReapHelperProcess(polite_child, pidfd.Get());
			const auto elapsed = std::chrono::steady_clock::now() - start;
			ok &= Expect(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM,
			             "helper terminated by SIGTERM is reaped with its status");
			ok &= Expect(elapsed < std::chrono::milliseconds(300),
			             "helper grace wait ends when the helper exits");
		}

		const pid_t exiting_child = SpawnExitingChild(5);
		ok &= Expect(exiting_child > 0, "spawns exiting helper");
		if (exiting_child > 0) {
			int        status = 0;
			const auto waited = WaitForHelperProcessUntil(
			    exiting_child, std::chrono::steady_clock::now() + std::chrono::seconds(5), &status,
			    -1);
			ok &= Expect(waited == HelperWaitResult::kExited && WIFEXITED(status) &&
			                 WEXITSTATUS(status) == 5,
			             "helper wait preserves exit status through the pidfd");
		}
		return ok;
	}

	auto ExpectSignalledChildStatus() -> bool {
		bool        ok        = true;
		const pid_t child_pid = SpawnBlockedChild();
		ok &= Expect(child_pid > 0, "spawns child to kill");
		if (child_pid <= 0) {
			return false;
		}
		ok &= Expect(kill(child_pid, SIGKILL) == 0, "kills compare child externally");
		const int status = howdy::test::prompt_coordinator::WaitUntilForTestChild(
		    child_pid, std::chrono::steady_clock::now() + std::chrono::seconds(5), nullptr, nullptr,
		    -1);
		ok &= Expect(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
		             "compare wait reports the terminating signal");
		ok &= Expect(ChildReaped(child_pid), "signalled compare child is reaped");
		return ok;
	}

	auto ExpectProcessWaiting() -> bool {
		bool        ok                = true;
		const pid_t compare_child_pid = SpawnExitingChild(7);
		ok &= Expect(compare_child_pid > 0, "spawns compare child");
		if (compare_child_pid > 0) {
			const int status = howdy::test::prompt_coordinator::WaitUntilForTestChild(
			    compare_child_pid, std::chrono::steady_clock::now() + std::chrono::seconds(1),
			    nullptr, nullptr, -1);
			ok &= Expect(WIFEXITED(status) && WEXITSTATUS(status) == 7,
			             "compare wait preserves child exit status");
		}

		const pid_t helper_child_pid = SpawnExitingChild(9);
		ok &= Expect(helper_child_pid > 0, "spawns helper child");
		if (helper_child_pid > 0) {
			int        status = 0;
			const auto waited =
			    howdy::pam::auth_helper_process::internal::WaitForHelperProcessUntil(
			        helper_child_pid, std::chrono::steady_clock::now() + std::chrono::seconds(5),
			        &status, -1);
			ok &= Expect(
			    waited == howdy::pam::auth_helper_process::internal::HelperWaitResult::kExited &&
			        WIFEXITED(status) && WEXITSTATUS(status) == 9,
			    "helper wait preserves child exit status");
		}

		constexpr pid_t nonexistent_child = std::numeric_limits<pid_t>::max();
		const int       compare_failure   = howdy::test::prompt_coordinator::WaitUntilForTestChild(
		    nonexistent_child, std::chrono::steady_clock::now() + std::chrono::seconds(1), nullptr,
		    nullptr, -1);
		const auto abort_code = static_cast<int>(howdy::native::CompareExit::kAbort);
		ok &= Expect(WIFEXITED(compare_failure) && WEXITSTATUS(compare_failure) == abort_code,
		             "compare wait failure returns abort status");
		int helper_failure_status = 0;
		ok &= Expect(howdy::pam::auth_helper_process::internal::WaitForHelperProcessUntil(
		                 nonexistent_child,
		                 std::chrono::steady_clock::now() + std::chrono::seconds(1),
		                 &helper_failure_status, -1) ==
		                 howdy::pam::auth_helper_process::internal::HelperWaitResult::kWaitError,
		             "helper wait failure is reported as an error");

		const pid_t cancelled_child = SpawnBlockedChild();
		ok &= Expect(cancelled_child > 0, "spawns cancellable compare child");
		if (cancelled_child > 0) {
			const int cancelled_status = howdy::test::prompt_coordinator::WaitUntilForTestChild(
			    cancelled_child, std::chrono::steady_clock::now() + std::chrono::seconds(1),
			    nullptr, AlwaysCancelCompare, -1);
			ok &= Expect(WIFEXITED(cancelled_status) && WEXITSTATUS(cancelled_status) == abort_code,
			             "compare cancellation returns abort status");
			ok &= Expect(ChildReaped(cancelled_child), "compare cancellation reaps child");
		}

		ok &= ExpectTerminalOutcomePreserved(false);
		ok &= ExpectTerminalOutcomePreserved(true);
		ok &= ExpectReapedElsewhereFailsClosed();
		ok &= ExpectSignalledChildStatus();
		ok &= ExpectHelperTrackedThroughPidfd();

		const pid_t cleanup_child = SpawnSigtermIgnoringChild();
		ok &= Expect(cleanup_child > 0, "spawns compare child for noexcept cleanup");
		if (cleanup_child > 0) {
			howdy::pam::compare_process::CancelAndReap(cleanup_child, -1);
			ok &= Expect(ChildReaped(cleanup_child), "noexcept compare cleanup reaps child");
		}

		const pid_t timed_out_child = SpawnSigtermIgnoringChild();
		ok &= Expect(timed_out_child > 0, "spawns SIGTERM-resistant compare child");
		if (timed_out_child > 0) {
			const int timed_out_status = howdy::test::prompt_coordinator::WaitUntilForTestChild(
			    timed_out_child, std::chrono::steady_clock::now(), nullptr, nullptr, -1);
			const int timeout_code = static_cast<int>(howdy::native::CompareExit::kTimeoutReached);
			ok &=
			    Expect(WIFEXITED(timed_out_status) && WEXITSTATUS(timed_out_status) == timeout_code,
				       "compare timeout kills SIGTERM-resistant child");
			ok &= Expect(ChildReaped(timed_out_child),
			             "compare timeout reaps SIGTERM-resistant child");
		}

		return ok;
	}

}  // namespace

auto RunProcessWaitTests() -> bool {
	return ExpectProcessWaiting();
}
