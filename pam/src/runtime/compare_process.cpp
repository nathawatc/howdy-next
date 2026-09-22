#ifndef _GNU_SOURCE
#	define _GNU_SOURCE
#endif

#include "runtime/compare_process.hpp"

#include "compare/args.hpp"
#include "paths.hpp"
#include "protocol/compare_exit.hpp"
#include "runtime/compare_launch.hpp"
#include "support/eintr.hpp"
#include "support/error_message.hpp"
#include "support/process_status.hpp"
#include "support/scoped_fd.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <sys/poll.h>
#include <sys/syslog.h>
#include <sys/types.h>
#include <sys/wait.h>

// Keep C linkage explicit for glibc headers without a C++ linkage guard.
extern "C" {
#include <sys/pidfd.h>
}

namespace {
	using CompareOperations   = howdy::pam::compare_process::Operations;
	using CompareSpawnRequest = howdy::pam::compare_process::SpawnRequest;

	// Lets compare process perform SIGTERM cleanup without extending scan deadline.
	constexpr auto kCompareTerminationGrace = std::chrono::milliseconds(250);

	using howdy::native::CompareExit;

	auto MakeCompareWaitExitStatus(CompareExit exit_code) -> int {
		return howdy::native::EncodeWaitExitStatus(exit_code);
	}

	enum class ChildState : std::uint8_t {
		kRunning,
		kReaped,
		kError
	};

	auto TryWaitForCompare(pid_t child_pid, int *status) -> ChildState {
		while (true) {
			const pid_t result = waitpid(child_pid, status, WNOHANG);
			if (result == child_pid) {
				return ChildState::kReaped;
			}
			if (result == 0) {
				return ChildState::kRunning;
			}
			if (errno == EINTR) {
				continue;
			}
			const int error_number = errno;
			syslog(LOG_ERR, "waitpid failed for compare process: %s (%d)",
			       howdy::native::SystemErrorMessage(error_number).c_str(), error_number);
			*status = MakeCompareWaitExitStatus(CompareExit::kAbort);
			return ChildState::kError;
		}
	}

	enum class WaitOutcome : std::uint8_t {
		kExited,
		kCancelled,
		kTimeout,
		kError
	};

	auto PollCompareUntil(int pidfd, int cancellation_fd,
	                      std::chrono::steady_clock::time_point deadline) -> WaitOutcome {
		using Clock = std::chrono::steady_clock;
		while (true) {
			const auto     remaining = std::max(deadline - Clock::now(), Clock::duration::zero());
			const auto     seconds   = std::chrono::duration_cast<std::chrono::seconds>(remaining);
			const timespec timeout{
			    .tv_sec  = seconds.count(),
			    .tv_nsec = std::chrono::duration_cast<std::chrono::nanoseconds>(remaining - seconds)
			                   .count()};
			std::array<pollfd, 2> descriptors{
			    {{.fd = pidfd, .events = POLLIN, .revents = 0},
			     {.fd = cancellation_fd, .events = POLLIN, .revents = 0}}};
			const int result = ppoll(descriptors.data(), descriptors.size(), &timeout, nullptr);
			if (result < 0) {
				if (errno == EINTR) {
					continue;
				}
				const int error_number = errno;
				syslog(LOG_ERR, "ppoll failed for compare process: %s (%d)",
				       howdy::native::SystemErrorMessage(error_number).c_str(), error_number);
				return WaitOutcome::kError;
			}
			for (const auto &descriptor : descriptors) {
				if ((static_cast<unsigned int>(descriptor.revents) &
				     static_cast<unsigned int>(POLLERR | POLLHUP | POLLNVAL)) != 0) {
					syslog(LOG_ERR, "Unexpected compare poll events on fd %d: %#x", descriptor.fd,
					       static_cast<unsigned int>(descriptor.revents));
					return WaitOutcome::kError;
				}
			}
			if ((static_cast<unsigned int>(descriptors.front().revents) & POLLIN) != 0) {
				return WaitOutcome::kExited;
			}
			if ((static_cast<unsigned int>(descriptors.back().revents) & POLLIN) != 0) {
				return WaitOutcome::kCancelled;
			}
			if (Clock::now() >= deadline) {
				return WaitOutcome::kTimeout;
			}
		}
	}

	auto ReapCompare(pid_t child_pid) -> std::optional<int> {
		int         status = 0;
		const pid_t result = howdy::native::RetryOnEintr([&] -> pid_t {
			return waitpid(child_pid, &status, 0);
		});
		if (result == child_pid) {
			return status;
		}
		const int error_number = errno;
		syslog(LOG_ERR, "waitpid failed while reaping compare process: %s (%d)",
		       howdy::native::SystemErrorMessage(error_number).c_str(), error_number);
		return std::nullopt;
	}

	auto SignalCompare(pid_t child_pid, int signal) -> bool {
		if (kill(child_pid, signal) != 0 && errno != ESRCH) {
			const int error_number = errno;
			syslog(LOG_ERR, "Failed to signal compare process (%d): %s (%d)", signal,
			       howdy::native::SystemErrorMessage(error_number).c_str(), error_number);
			return false;
		}
		return true;
	}

	auto OpenComparePidfd(pid_t child_pid) -> howdy::native::ScopedFd {
		howdy::native::ScopedFd pidfd(pidfd_open(child_pid, 0));
		if (!pidfd.Valid()) {
			const int error_number = errno;
			syslog(LOG_ERR, "pidfd_open failed for compare process: %s (%d)",
			       howdy::native::SystemErrorMessage(error_number).c_str(), error_number);
			return {};
		}
		if (pidfd.Get() <= STDERR_FILENO) {
			const int normalized = fcntl(pidfd.Get(), F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
			if (normalized < 0) {
				const int error_number = errno;
				syslog(LOG_ERR, "Compare pidfd normalization failed: %s (%d)",
				       howdy::native::SystemErrorMessage(error_number).c_str(), error_number);
				return {};
			}
			pidfd.Reset(normalized);
		}
		return pidfd;
	}

	auto WaitForCompareProcess(pid_t child_pid, std::chrono::steady_clock::time_point deadline,
	                           void                                      *cancellation_context,
	                           howdy::pam::CompareCancellationRequestedFn cancellation_requested,
	                           int                                        cancellation_fd) -> int {
		using Clock            = std::chrono::steady_clock;
		const int abort_status = MakeCompareWaitExitStatus(CompareExit::kAbort);
		if (child_pid <= 0) {
			syslog(LOG_ERR, "Invalid compare child pid: %d", child_pid);
			return abort_status;
		}
		const auto pidfd = OpenComparePidfd(child_pid);
		if (!pidfd.Valid()) {
			int status = 0;
			if (TryWaitForCompare(child_pid, &status) == ChildState::kRunning) {
				(void)SignalCompare(child_pid, SIGKILL);
				(void)ReapCompare(child_pid);
			}
			return abort_status;
		}

		// Preserve exit-before-cancellation precedence and the terminal decision once made.
		int status = 0;
		if (TryWaitForCompare(child_pid, &status) != ChildState::kRunning) {
			return status;
		}
		WaitOutcome outcome = WaitOutcome::kTimeout;
		if (cancellation_requested != nullptr && cancellation_requested(cancellation_context)) {
			outcome = WaitOutcome::kCancelled;
		} else if (Clock::now() < deadline) {
			outcome = PollCompareUntil(pidfd.Get(), cancellation_fd, deadline);
		}
		if (outcome == WaitOutcome::kExited) {
			return ReapCompare(child_pid).value_or(abort_status);
		}
		int        terminal_status = outcome == WaitOutcome::kTimeout
		                                 ? MakeCompareWaitExitStatus(CompareExit::kTimeoutReached)
		                                 : abort_status;
		const auto child_state     = TryWaitForCompare(child_pid, &status);
		if (child_state != ChildState::kRunning) {
			return child_state == ChildState::kReaped ? terminal_status : abort_status;
		}
		if (outcome != WaitOutcome::kError) {
			if (!SignalCompare(child_pid, SIGTERM)) {
				terminal_status = abort_status;
			}
			const auto grace =
			    PollCompareUntil(pidfd.Get(), -1, Clock::now() + kCompareTerminationGrace);
			if (grace == WaitOutcome::kExited) {
				return ReapCompare(child_pid).has_value() ? terminal_status : abort_status;
			}
			if (grace == WaitOutcome::kError) {
				terminal_status = abort_status;
			}
		}
		if (!SignalCompare(child_pid, SIGKILL)) {
			terminal_status = abort_status;
		}
		return ReapCompare(child_pid).has_value() ? terminal_status : abort_status;
	}

	auto CallPosixSpawnFileActionsInit(void *context, posix_spawn_file_actions_t *actions) -> int {
		(void)context;
		return posix_spawn_file_actions_init(actions);
	}

	auto CallPosixSpawnFileActionsAddclosefrom(void *context, posix_spawn_file_actions_t *actions,
	                                           int from_fd) -> int {
		(void)context;
		return posix_spawn_file_actions_addclosefrom_np(actions, from_fd);
	}

	auto CallPosixSpawnFileActionsDestroy(void *context, posix_spawn_file_actions_t *actions)
	    -> int {
		(void)context;
		return posix_spawn_file_actions_destroy(actions);
	}

	auto CallPosixSpawn(const CompareSpawnRequest &request) -> int {
		(void)request.context;
		return posix_spawn(request.child_pid, request.path, request.actions, nullptr,
		                   request.argv.data(), request.envp.data());
	}

	constexpr CompareOperations kPosixSpawnOperations = {
	    .context                   = nullptr,
	    .file_actions_init         = CallPosixSpawnFileActionsInit,
	    .file_actions_addclosefrom = CallPosixSpawnFileActionsAddclosefrom,
	    .file_actions_destroy      = CallPosixSpawnFileActionsDestroy,
	    .spawn                     = CallPosixSpawn,
	};

	auto SpawnCompareProcess(const howdy::pam::CompareLaunchRequest &request, pid_t *child_pid,
	                         const CompareOperations &operations) -> int {
		std::string compare_path(kCompareProcessPath);
		std::string config_option(howdy::native::kCompareConfigOption);
		std::string config_path(request.config_path);
		std::string username(request.username);

		std::array<char *, 5> args = {compare_path.data(), config_option.data(), config_path.data(),
		                              username.data(), nullptr};

		std::string user_models_env =
		    "HOWDY_USER_MODELS_DIR=" + std::string(request.user_models_dir);

		std::array<char *, 2> runtime_env = {
		    user_models_env.data(),
		    nullptr,
		};
		std::array<char *, 1> empty_env = {
		    nullptr,
		};

		const std::span<char *const> compare_env = request.staged_runtime
		                                               ? std::span<char *const>(runtime_env)
		                                               : std::span<char *const>(empty_env);

		posix_spawn_file_actions_t file_actions;
		const int init_result = operations.file_actions_init(operations.context, &file_actions);
		if (init_result != 0) {
			return init_result;
		}

		const int closefrom_result = operations.file_actions_addclosefrom(
		    operations.context, &file_actions, STDERR_FILENO + 1);
		if (closefrom_result != 0) {
			(void)operations.file_actions_destroy(operations.context, &file_actions);
			return closefrom_result;
		}

		const int spawn_result = operations.spawn({.context   = operations.context,
		                                           .child_pid = child_pid,
		                                           .path      = kCompareProcessPath,
		                                           .actions   = &file_actions,
		                                           .argv      = args,
		                                           .envp      = compare_env});
		(void)operations.file_actions_destroy(operations.context, &file_actions);
		return spawn_result;
	}

}  // namespace

namespace howdy::pam::compare_process {

	auto ProductionOperations() -> CompareOperations {
		return kPosixSpawnOperations;
	}

	auto Spawn(const CompareLaunchRequest &request, pid_t *child_pid,
	           const CompareOperations &operations) -> int {
		return SpawnCompareProcess(request, child_pid, operations);
	}

	auto WaitUntil(pid_t child_pid, std::chrono::steady_clock::time_point deadline) -> int {
		return WaitForCompareProcess(child_pid, deadline, nullptr, nullptr, -1);
	}

	auto WaitUntil(pid_t child_pid, std::chrono::steady_clock::time_point deadline,
	               void                          *cancellation_context,
	               CompareCancellationRequestedFn cancellation_requested, int cancellation_fd)
	    -> int {
		return WaitForCompareProcess(child_pid, deadline, cancellation_context,
		                             cancellation_requested, cancellation_fd);
	}

	auto Wait(void *context, pid_t child_pid, std::chrono::steady_clock::time_point deadline,
	          void *cancellation_context, CompareCancellationRequestedFn cancellation_requested,
	          int cancellation_fd) -> int {
		(void)context;
		return WaitUntil(child_pid, deadline, cancellation_context, cancellation_requested,
		                 cancellation_fd);
	}

	void CancelAndReap(pid_t child_pid) noexcept {
		try {
			(void)WaitForCompareProcess(child_pid, std::chrono::steady_clock::now(), nullptr,
			                            nullptr, -1);
		} catch (...) {
			(void)kill(child_pid, SIGKILL);
			int status = 0;
			while (waitpid(child_pid, &status, 0) < 0 && errno == EINTR) {
			}
		}
	}

}  // namespace howdy::pam::compare_process
