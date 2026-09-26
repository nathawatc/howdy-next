#ifndef _GNU_SOURCE
#	define _GNU_SOURCE
#endif

#include "internal.hpp"
#include "protocol/compare_exit.hpp"
#include "support/error_message.hpp"
#include "support/process_status.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>

#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/poll.h>
#include <sys/syslog.h>
#include <sys/types.h>
#include <sys/wait.h>

namespace {

	using howdy::native::CompareExit;
	using howdy::pam::auth_helper_process::internal::HelperDeadline;

	constexpr auto kHelperWaitPollInterval = std::chrono::milliseconds(10);

	auto MakeAuthHelperWaitExitStatus(CompareExit exit_code) -> int {
		return howdy::native::EncodeWaitExitStatus(exit_code);
	}

}  // namespace

namespace howdy::pam::auth_helper_process::internal {

	void LogAuthHelperReadError(int error_number) {
		syslog(LOG_AUTHPRIV | LOG_ERR, "pam_howdy: Failed to read auth helper output: %s (%d)",
		       howdy::native::SystemErrorMessage(error_number).c_str(), error_number);
	}

	auto DeadlinePollTimeout(HelperDeadline deadline) -> int {
		const auto remaining = deadline - std::chrono::steady_clock::now();
		if (remaining <= HelperDeadline::duration::zero()) {
			return 0;
		}
		return static_cast<int>(std::chrono::ceil<std::chrono::milliseconds>(remaining).count());
	}

	auto WaitForHelperProcess(pid_t child_pid) noexcept -> int {
		while (true) {
			int         status      = 0;
			const pid_t wait_result = waitpid(child_pid, &status, 0);
			if (wait_result == child_pid) {
				return status;
			}
			if (wait_result < 0 && errno == EINTR) {
				continue;
			}
			return MakeAuthHelperWaitExitStatus(CompareExit::kAbort);
		}
	}

	auto TerminateAndReapHelperProcess(pid_t child_pid) noexcept -> int {
		int         status              = 0;
		const pid_t initial_wait_result = waitpid(child_pid, &status, WNOHANG);
		if (initial_wait_result == child_pid) {
			return status;
		}
		if (initial_wait_result < 0 && errno == ECHILD) {
			return MakeAuthHelperWaitExitStatus(CompareExit::kAbort);
		}

		if (kill(child_pid, SIGTERM) != 0) {
			const int error_number = errno;
			if (error_number != ESRCH) {
				syslog(LOG_AUTHPRIV | LOG_WARNING,
				       "pam_howdy: Failed to terminate auth helper process (%d)", error_number);
			}
		}

		for (int attempts = 0; attempts < 50; ++attempts) {
			status                  = 0;
			const pid_t wait_result = waitpid(child_pid, &status, WNOHANG);
			if (wait_result == child_pid) {
				return status;
			}
			if (wait_result < 0) {
				if (errno == EINTR) {
					continue;
				}
				return MakeAuthHelperWaitExitStatus(CompareExit::kAbort);
			}
			usleep(10000);
		}

		if (kill(child_pid, SIGKILL) != 0) {
			const int error_number = errno;
			if (error_number != ESRCH) {
				syslog(LOG_AUTHPRIV | LOG_WARNING,
				       "pam_howdy: Failed to kill auth helper process (%d)", error_number);
			}
		}
		return WaitForHelperProcess(child_pid);
	}

	auto WaitForHelperProcessUntil(pid_t child_pid, HelperDeadline deadline, int *status) noexcept
	    -> HelperWaitResult {
		while (true) {
			const pid_t wait_result = waitpid(child_pid, status, WNOHANG);
			if (wait_result == child_pid) {
				return std::chrono::steady_clock::now() >= deadline ? HelperWaitResult::kTimedOut
				                                                    : HelperWaitResult::kExited;
			}
			if (wait_result < 0) {
				if (errno == EINTR) {
					continue;
				}
				if (errno != ECHILD) {
					(void)TerminateAndReapHelperProcess(child_pid);
				}
				return HelperWaitResult::kWaitError;
			}
			if (std::chrono::steady_clock::now() >= deadline) {
				(void)TerminateAndReapHelperProcess(child_pid);
				return HelperWaitResult::kTimedOut;
			}
			const auto wake_time =
			    std::min(deadline, std::chrono::steady_clock::now() + kHelperWaitPollInterval);
			(void)poll(nullptr, 0, DeadlinePollTimeout(wake_time));
		}
	}

}  // namespace howdy::pam::auth_helper_process::internal
