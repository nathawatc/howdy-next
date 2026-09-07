#ifndef _GNU_SOURCE
#	define _GNU_SOURCE
#endif

#include "internal.hpp"
#include "protocol/compare_exit.hpp"
#include "support/fd_io.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <poll.h>
#include <string>
#include <syslog.h>
#include <unistd.h>

#include <sys/wait.h>

namespace {

	using howdy::native::CompareExit;
	using howdy::pam::auth_helper_process::internal::HelperDeadline;
	using howdy::pam::auth_helper_process::internal::HelperReadResult;

	constexpr auto kHelperWaitPollInterval = std::chrono::milliseconds(10);

	auto MakeAuthHelperWaitExitStatus(CompareExit exit_code) -> int {
		return static_cast<int>(exit_code) << 8;
	}

	enum class HelperPollResult : std::uint8_t {
		kReady,
		kRetry,
		kTimedOut,
		kError,
	};

	auto PollAuthHelperOutput(int output_fd, HelperDeadline deadline) -> HelperPollResult {
		pollfd    descriptor{.fd = output_fd, .events = POLLIN, .revents = 0};
		const int poll_result =
		    poll(&descriptor, 1,
		         howdy::pam::auth_helper_process::internal::DeadlinePollTimeout(deadline));
		if (poll_result < 0) {
			if (errno == EINTR) {
				return HelperPollResult::kRetry;
			}
			const int read_error = errno;
			howdy::pam::auth_helper_process::internal::LogAuthHelperReadError(read_error);
			return HelperPollResult::kError;
		}
		if (poll_result == 0) {
			return HelperPollResult::kTimedOut;
		}
		if ((descriptor.revents & (POLLNVAL | POLLERR)) != 0) {
			const int read_error = (descriptor.revents & POLLNVAL) != 0 ? EBADF : EIO;
			howdy::pam::auth_helper_process::internal::LogAuthHelperReadError(read_error);
			return HelperPollResult::kError;
		}
		return (descriptor.revents & (POLLIN | POLLHUP)) == 0 ? HelperPollResult::kRetry
		                                                      : HelperPollResult::kReady;
	}

	auto ReadAuthHelperOutputFromFd(int output_fd, std::string &output, HelperDeadline deadline)
	    -> HelperReadResult {
		if (output_fd < 0) {
			constexpr int read_error = EBADF;
			howdy::pam::auth_helper_process::internal::LogAuthHelperReadError(read_error);
			return HelperReadResult::kReadError;
		}

		std::array<char, 4096> buffer{};
		while (true) {
			const auto poll_result = PollAuthHelperOutput(output_fd, deadline);
			if (poll_result == HelperPollResult::kRetry) {
				continue;
			}
			if (poll_result == HelperPollResult::kTimedOut) {
				output.clear();
				return HelperReadResult::kTimedOut;
			}
			if (poll_result == HelperPollResult::kError) {
				output.clear();
				return HelperReadResult::kReadError;
			}

			const ssize_t bytes_read = read(output_fd, buffer.data(), buffer.size());
			if (bytes_read > 0) {
				if (output.size() + static_cast<std::size_t>(bytes_read) >=
				    howdy::pam::auth_helper_process::internal::kAuthHelperOutputLimit) {
					output.clear();
					return HelperReadResult::kOutputLimit;
				}
				output.append(buffer.data(), static_cast<std::size_t>(bytes_read));
				continue;
			}
			if (bytes_read == 0) {
				if (std::chrono::steady_clock::now() >= deadline) {
					output.clear();
					return HelperReadResult::kTimedOut;
				}
				return HelperReadResult::kComplete;
			}
			if (errno != EINTR && errno != EAGAIN) {
				const int read_error = errno;
				output.clear();
				howdy::pam::auth_helper_process::internal::LogAuthHelperReadError(read_error);
				return HelperReadResult::kReadError;
			}
		}
	}

}  // namespace

namespace howdy::pam::auth_helper_process::internal {

	void LogAuthHelperReadError(int error_number) {
		syslog(LOG_ERR, "Failed to read auth helper output: %s (%d)", strerror(error_number),
		       error_number);
	}

	auto DeadlinePollTimeout(HelperDeadline deadline) -> int {
		const auto remaining = deadline - std::chrono::steady_clock::now();
		if (remaining <= HelperDeadline::duration::zero()) {
			return 0;
		}
		return static_cast<int>(std::chrono::ceil<std::chrono::milliseconds>(remaining).count());
	}

	auto WaitForHelperProcess(pid_t child_pid) -> int {
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

	auto TerminateAndReapHelperProcess(pid_t child_pid) -> int {
		int         status              = 0;
		const pid_t initial_wait_result = waitpid(child_pid, &status, WNOHANG);
		if (initial_wait_result == child_pid) {
			return status;
		}
		if (initial_wait_result < 0 && errno == ECHILD) {
			return MakeAuthHelperWaitExitStatus(CompareExit::kAbort);
		}

		if (kill(child_pid, SIGTERM) != 0 && errno != ESRCH) {
			syslog(LOG_WARNING, "Failed to terminate auth helper process: %s (%d)", strerror(errno),
			       errno);
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

		if (kill(child_pid, SIGKILL) != 0 && errno != ESRCH) {
			syslog(LOG_WARNING, "Failed to kill auth helper process: %s (%d)", strerror(errno),
			       errno);
		}
		return WaitForHelperProcess(child_pid);
	}

	auto WaitForHelperProcessUntil(pid_t child_pid, HelperDeadline deadline, int *status)
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

	auto ReadAuthHelperOutputUntil(int output_fd, std::string *output,
	                               const howdy::pam::auth_helper_process::Operations &operations,
	                               HelperDeadline deadline) -> HelperReadResult {
		if (output == nullptr) {
			return HelperReadResult::kReadError;
		}
		output->clear();

		if (operations.read_bounded != nullptr) {
			const auto helper_output = operations.read_bounded(
			    operations.context, {.fd = output_fd, .max_bytes = kAuthHelperOutputLimit});
			if (helper_output.read_error) {
				LogAuthHelperReadError(helper_output.error_number);
				return HelperReadResult::kReadError;
			}
			if (helper_output.hit_limit) {
				return HelperReadResult::kOutputLimit;
			}
			if (std::chrono::steady_clock::now() >= deadline) {
				output->clear();
				return HelperReadResult::kTimedOut;
			}
			*output = helper_output.output;
			return HelperReadResult::kComplete;
		}

		return ReadAuthHelperOutputFromFd(output_fd, *output, deadline);
	}

}  // namespace howdy::pam::auth_helper_process::internal
