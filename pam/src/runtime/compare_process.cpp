#ifndef _GNU_SOURCE
#	define _GNU_SOURCE
#endif

#include "runtime/compare_process.hpp"

#include "paths.hpp"
#include "protocol/compare_exit.hpp"
#include "protocol/compare_invocation.hpp"
#include "runtime/child_process.hpp"
#include "runtime/compare_launch.hpp"
#include "support/bit_flags.hpp"
#include "support/error_message.hpp"
#include "support/process_status.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <sys/poll.h>
#include <sys/syslog.h>
#include <sys/types.h>

namespace {
	using CompareOperations   = howdy::pam::compare_process::Operations;
	using CompareSpawnRequest = howdy::pam::compare_process::SpawnRequest;

	// Fixed compare environment entries that keep a threaded BLAS from reserving address space per
	// CPU thread. OpenBLAS built with OpenMP (Arch) obeys OMP_NUM_THREADS and ignores
	// OPENBLAS_NUM_THREADS; pthread builds obey OPENBLAS_NUM_THREADS.
	constexpr auto kCompareThreadPins = std::to_array<std::string_view>({
	    "OPENBLAS_NUM_THREADS=1",
	    "OMP_NUM_THREADS=1",
	});

	// Lets compare process perform SIGTERM cleanup without extending scan deadline.
	constexpr auto kCompareTerminationGrace = std::chrono::milliseconds(250);

	using howdy::native::CompareExit;

	auto MakeCompareWaitExitStatus(CompareExit exit_code) -> int {
		return howdy::native::EncodeWaitExitStatus(exit_code);
	}

	using howdy::pam::runtime::ChildState;

	// The compare child with this module's logging and status policy; the pidfd and PID-fallback
	// rules live in runtime::TrackedChild.
	class CompareChild {
	public:
		CompareChild(pid_t pid, int pidfd)
		    : child_(pid, pidfd) {}

		// Non-blocking. On kError `status` holds the abort status, and a child that may still be
		// running (any failure but ECHILD) has been stopped, as the helper path does.
		auto TryWait(int *status) -> ChildState {
			const auto state = child_.Wait(false, status);
			if (state == ChildState::kError) {
				const int error_number = errno;
				syslog(LOG_AUTHPRIV | LOG_ERR,
				       "pam_howdy: wait failed for compare process: %s (%d)",
				       howdy::native::SystemErrorMessage(error_number).c_str(), error_number);
				if (error_number != ECHILD) {
					(void)howdy::pam::runtime::KillAndReap(child_);
				}
				*status = MakeCompareWaitExitStatus(CompareExit::kAbort);
			}
			return state;
		}

		auto Reap() -> std::optional<int> {
			int status = 0;
			if (child_.Wait(true, &status) == ChildState::kReaped) {
				return status;
			}
			const int error_number = errno;
			syslog(LOG_AUTHPRIV | LOG_ERR,
			       "pam_howdy: wait failed while reaping compare process: %s (%d)",
			       howdy::native::SystemErrorMessage(error_number).c_str(), error_number);
			return std::nullopt;
		}

		// False means the child could not be signalled at all; do not block on it afterwards.
		[[nodiscard]] auto Signal(int signal) -> bool {
			const int error_number = child_.Signal(signal);
			if (error_number == 0) {
				return true;
			}
			// ECHILD: reaped elsewhere; the wait that follows reports it once.
			if (error_number == ECHILD) {
				return false;
			}
			syslog(LOG_AUTHPRIV | LOG_ERR,
			       "pam_howdy: Failed to signal compare process (%d): %s (%d)", signal,
			       howdy::native::SystemErrorMessage(error_number).c_str(), error_number);
			return false;
		}

	private:
		howdy::pam::runtime::TrackedChild child_;
	};

	enum class WaitOutcome : std::uint8_t {
		kExited,
		kCancelled,
		kTimeout,
		kError,
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
				               .count(),
			};
			std::array<pollfd, 2> descriptors{
			    {
			        {.fd = pidfd, .events = POLLIN, .revents = 0},
			        {.fd = cancellation_fd, .events = POLLIN, .revents = 0},
			    },
			};
			const int result = ppoll(descriptors.data(), descriptors.size(), &timeout, nullptr);
			if (result < 0) {
				if (errno == EINTR) {
					continue;
				}
				const int error_number = errno;
				syslog(LOG_AUTHPRIV | LOG_ERR,
				       "pam_howdy: ppoll failed for compare process: %s (%d)",
				       howdy::native::SystemErrorMessage(error_number).c_str(), error_number);
				return WaitOutcome::kError;
			}
			const auto pidfd_events        = descriptors.front().revents;
			const auto cancellation_events = descriptors.back().revents;
			// POLLHUP on a pidfd means the child has been reaped (Linux 6.9+), which the reap that
			// follows reports; on the cancellation eventfd it is an error.
			if (howdy::native::HasAnyBits(pidfd_events, POLLERR, POLLNVAL) ||
			    howdy::native::HasAnyBits(cancellation_events, POLLERR, POLLHUP, POLLNVAL)) {
				syslog(LOG_AUTHPRIV | LOG_ERR,
				       "pam_howdy: Unexpected compare poll events: pidfd %#x, cancellation %#x",
				       static_cast<unsigned int>(pidfd_events),
				       static_cast<unsigned int>(cancellation_events));
				return WaitOutcome::kError;
			}
			if (howdy::native::HasAnyBits(pidfd_events, POLLIN, POLLHUP)) {
				return WaitOutcome::kExited;
			}
			if (howdy::native::HasAnyBits(cancellation_events, POLLIN)) {
				return WaitOutcome::kCancelled;
			}
			if (Clock::now() >= deadline) {
				return WaitOutcome::kTimeout;
			}
		}
	}

	void KillAndReapCompare(pid_t child_pid, int child_pidfd) noexcept {
		howdy::pam::runtime::TrackedChild child(child_pid, child_pidfd);
		(void)howdy::pam::runtime::KillAndReap(child);
	}

	// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
	auto WaitForCompareProcess(pid_t child_pid, int child_pidfd,
	                           std::chrono::steady_clock::time_point      deadline,
	                           void                                      *cancellation_context,
	                           howdy::pam::CompareCancellationRequestedFn cancellation_requested,
	                           int                                        cancellation_fd) -> int {
		using Clock            = std::chrono::steady_clock;
		const int abort_status = MakeCompareWaitExitStatus(CompareExit::kAbort);
		if (child_pid <= 0) {
			syslog(LOG_AUTHPRIV | LOG_ERR, "pam_howdy: Invalid compare child pid: %d", child_pid);
			return abort_status;
		}
		// The pidfd taken at spawn is borrowed and required: it is what the wait polls, and one
		// opened now could name a process that reused the PID. Without it, stop the child by PID
		// and fail closed.
		if (child_pidfd < 0) {
			syslog(LOG_AUTHPRIV | LOG_ERR, "pam_howdy: No pidfd for compare process %d", child_pid);
			KillAndReapCompare(child_pid, -1);
			return abort_status;
		}

		// Preserve exit-before-cancellation precedence and the terminal decision once made.
		CompareChild child(child_pid, child_pidfd);
		int          status = 0;
		if (child.TryWait(&status) != ChildState::kRunning) {
			return status;
		}
		WaitOutcome outcome = WaitOutcome::kTimeout;
		if (cancellation_requested != nullptr && cancellation_requested(cancellation_context)) {
			outcome = WaitOutcome::kCancelled;
		} else if (Clock::now() < deadline) {
			outcome = PollCompareUntil(child_pidfd, cancellation_fd, deadline);
		}
		if (outcome == WaitOutcome::kExited) {
			return child.Reap().value_or(abort_status);
		}
		int        terminal_status = outcome == WaitOutcome::kTimeout
		                                 ? MakeCompareWaitExitStatus(CompareExit::kTimeoutReached)
		                                 : abort_status;
		const auto child_state     = child.TryWait(&status);
		if (child_state != ChildState::kRunning) {
			return child_state == ChildState::kReaped ? terminal_status : abort_status;
		}
		if (outcome != WaitOutcome::kError) {
			if (!child.Signal(SIGTERM)) {
				terminal_status = abort_status;
			}
			const auto grace =
			    PollCompareUntil(child_pidfd, -1, Clock::now() + kCompareTerminationGrace);
			if (grace == WaitOutcome::kExited) {
				return child.Reap().has_value() ? terminal_status : abort_status;
			}
			if (grace == WaitOutcome::kError) {
				terminal_status = abort_status;
			}
		}
		if (!child.Signal(SIGKILL)) {
			// A blocking reap would wait forever on a child that was never signalled; still
			// collect it if it has exited meanwhile.
			(void)child.TryWait(&status);
			return abort_status;
		}
		return child.Reap().has_value() ? terminal_status : abort_status;
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

		std::array<char *, 5> args = {
		    compare_path.data(), config_option.data(), config_path.data(), username.data(), nullptr,
		};

		std::string user_models_env =
		    "HOWDY_USER_MODELS_DIR=" + std::string(request.user_models_dir);

		// Compare never calls BLAS, but a threaded BLAS linked into OpenCV starts its thread pool
		// when the library loads and reserves address space per thread (about 128 MiB per OpenMP
		// thread with Arch's OpenBLAS), which can exhaust the compare RLIMIT_AS. The pool is
		// created before main(), so the environment is the only place to limit it. These are fixed
		// values, never copied from the caller. If compare starts using BLAS-heavy OpenCV calls,
		// revisit this.
		auto                pins = std::ranges::to<std::vector<std::string>>(kCompareThreadPins);
		std::vector<char *> environment;
		environment.reserve(pins.size() + 2);
		if (request.staged_runtime) {
			environment.push_back(user_models_env.data());
		}
		for (auto &pin : pins) {
			environment.push_back(pin.data());
		}
		environment.push_back(nullptr);
		const std::span<char *const> compare_env(environment);

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

		const int spawn_result = operations.spawn({
		    .context   = operations.context,
		    .child_pid = child_pid,
		    .path      = kCompareProcessPath,
		    .actions   = &file_actions,
		    .argv      = args,
		    .envp      = compare_env,
		});
		(void)operations.file_actions_destroy(operations.context, &file_actions);
		return spawn_result;
	}

}  // namespace

namespace howdy::pam::compare_process {

	auto ProductionOperations() -> CompareOperations {
		return kPosixSpawnOperations;
	}

	auto Spawn(const CompareLaunchRequest &request, pid_t *child_pid, int *child_pidfd,
	           const CompareOperations &operations) -> int {
		if (child_pid == nullptr || child_pidfd == nullptr) {
			return EINVAL;  // the pidfd is required, so there is nowhere to put a child without it
		}
		*child_pidfd     = -1;
		const int result = SpawnCompareProcess(request, child_pid, operations);
		// Taken before the PAM application can reap compare and let the PID be reused.
		if (result == 0 && *child_pid > 0) {
			// A pidfd opened later could name another process, so a failure fails the spawn.
			int error_number = 0;
			*child_pidfd = howdy::pam::runtime::TakeSpawnedChildPidfd(*child_pid, &error_number);
			if (*child_pidfd < 0) {
				syslog(LOG_AUTHPRIV | LOG_ERR,
				       "pam_howdy: Could not take a pidfd for compare process: %s (%d)",
				       howdy::native::SystemErrorMessage(error_number).c_str(), error_number);
				*child_pid = -1;
				return error_number;
			}
		}
		return result;
	}

	auto WaitUntil(pid_t child_pid, int child_pidfd, std::chrono::steady_clock::time_point deadline,
	               void                          *cancellation_context,
	               CompareCancellationRequestedFn cancellation_requested, int cancellation_fd)
	    -> int {
		return WaitForCompareProcess(child_pid, child_pidfd, deadline, cancellation_context,
		                             cancellation_requested, cancellation_fd);
	}

	auto Wait(void *context, pid_t child_pid, int child_pidfd,
	          std::chrono::steady_clock::time_point deadline, void *cancellation_context,
	          CompareCancellationRequestedFn cancellation_requested, int cancellation_fd) -> int {
		(void)context;
		return WaitUntil(child_pid, child_pidfd, deadline, cancellation_context,
		                 cancellation_requested, cancellation_fd);
	}

	void CancelAndReap(pid_t child_pid, int child_pidfd) noexcept {
		try {
			(void)WaitForCompareProcess(child_pid, child_pidfd, std::chrono::steady_clock::now(),
			                            nullptr, nullptr, -1);
		} catch (...) {
			KillAndReapCompare(child_pid, child_pidfd);
		}
	}

}  // namespace howdy::pam::compare_process
