#include "protocol/compare_exit.hpp"
#include "runtime/auth_helper_process.hpp"
#include "runtime/compare_process.hpp"
#include "support/process_test_support.hpp"
#include "test_support.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <string>
#include <unistd.h>

#include <sys/wait.h>

auto RunProcessWaitTests() -> bool;

namespace {
	using namespace howdy::test::process;
	using howdy::test::Expect;
	using howdy::test::ScopedFd;

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
		if (pipe(release_pipe.data()) != 0) {
			return -1;
		}

		const pid_t child_pid = fork();
		if (child_pid < 0) {
			(void)close(release_pipe[0]);
			(void)close(release_pipe[1]);
			return -1;
		}
		if (child_pid == 0) {
			(void)close(release_pipe[1]);
			char    release = '\0';
			ssize_t read_result;
			do {
				read_result = read(release_pipe[0], &release, sizeof(release));
			} while (read_result < 0 && errno == EINTR);
			(void)close(release_pipe[0]);
			_exit(read_result == 1 && release == '1' ? EXIT_SUCCESS : EXIT_FAILURE);
		}

		(void)close(release_pipe[0]);
		*release_fd = release_pipe[1];
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
		auto      &boundary = *static_cast<TerminalBoundaryChild *>(context);
		const char release  = '1';
		ssize_t    write_result;
		do {
			write_result = write(boundary.release_fd, &release, sizeof(release));
		} while (write_result < 0 && errno == EINTR);
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
		const int  status   = howdy::pam::compare_process::WaitUntil(child_pid, deadline, &boundary,
		                                                             ReleaseTerminalBoundaryChild);
		const auto expected_exit   = cancellation ? howdy::native::CompareExit::kAbort
		                                          : howdy::native::CompareExit::kTimeoutReached;
		const int  expected_status = static_cast<int>(expected_exit) << 8;
		ok &= Expect(boundary.child_exit_observed,
		             outcome + " boundary observes child exit before cleanup wait");
		ok &= Expect(WIFEXITED(status) && status == expected_status,
		             outcome + " preserves terminal outcome after child exit");
		ok &= Expect(ChildReaped(child_pid), outcome + " boundary reaps child");
		return ok;
	}

	auto ExpectProcessWaiting() -> bool {
		bool        ok                = true;
		const pid_t compare_child_pid = SpawnExitingChild(7);
		ok &= Expect(compare_child_pid > 0, "spawns compare child");
		if (compare_child_pid > 0) {
			const int status = howdy::pam::compare_process::WaitUntil(
			    compare_child_pid, std::chrono::steady_clock::now() + std::chrono::seconds(1));
			ok &= Expect(WIFEXITED(status) && WEXITSTATUS(status) == 7,
			             "compare wait preserves child exit status");
		}

		const pid_t helper_child_pid = SpawnExitingChild(9);
		ok &= Expect(helper_child_pid > 0, "spawns helper child");
		if (helper_child_pid > 0) {
			const int status = howdy::pam::auth_helper_process::WaitForHelper(helper_child_pid);
			ok &= Expect(WIFEXITED(status) && WEXITSTATUS(status) == 9,
			             "helper wait preserves child exit status");
		}

		constexpr pid_t nonexistent_child = std::numeric_limits<pid_t>::max();
		const int       compare_failure   = howdy::pam::compare_process::WaitUntil(
		    nonexistent_child, std::chrono::steady_clock::now() + std::chrono::seconds(1));
		const auto abort_code = static_cast<int>(howdy::native::CompareExit::kAbort);
		ok &= Expect(WIFEXITED(compare_failure) && WEXITSTATUS(compare_failure) == abort_code,
		             "compare wait failure returns abort status");
		const int helper_failure =
		    howdy::pam::auth_helper_process::WaitForHelper(nonexistent_child);
		ok &= Expect(WIFEXITED(helper_failure) && WEXITSTATUS(helper_failure) == abort_code,
		             "helper wait failure returns abort status");

		const pid_t cancelled_child = SpawnBlockedChild();
		ok &= Expect(cancelled_child > 0, "spawns cancellable compare child");
		if (cancelled_child > 0) {
			const int cancelled_status = howdy::pam::compare_process::WaitUntil(
			    cancelled_child, std::chrono::steady_clock::now() + std::chrono::seconds(1),
			    nullptr, AlwaysCancelCompare);
			ok &= Expect(WIFEXITED(cancelled_status) && WEXITSTATUS(cancelled_status) == abort_code,
			             "compare cancellation returns abort status");
			ok &= Expect(ChildReaped(cancelled_child), "compare cancellation reaps child");
		}

		ok &= ExpectTerminalOutcomePreserved(false);
		ok &= ExpectTerminalOutcomePreserved(true);

		const pid_t timed_out_child = SpawnSigtermIgnoringChild();
		ok &= Expect(timed_out_child > 0, "spawns SIGTERM-resistant compare child");
		if (timed_out_child > 0) {
			const int timed_out_status = howdy::pam::compare_process::WaitUntil(
			    timed_out_child, std::chrono::steady_clock::now());
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
