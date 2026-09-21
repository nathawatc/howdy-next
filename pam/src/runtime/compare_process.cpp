#ifndef _GNU_SOURCE
#	define _GNU_SOURCE
#endif

#include "runtime/compare_process.hpp"

#include "compare/args.hpp"
#include "paths.hpp"
#include "protocol/compare_exit.hpp"
#include "runtime/compare_launch.hpp"
#include "support/error_message.hpp"
#include "support/process_status.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <optional>
#include <span>
#include <string>
#include <thread>

#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/syslog.h>
#include <sys/types.h>
#include <sys/wait.h>

namespace {
	using CompareOperations   = howdy::pam::compare_process::Operations;
	using CompareSpawnRequest = howdy::pam::compare_process::SpawnRequest;

	constexpr auto kCompareWaitPollInterval = std::chrono::milliseconds(10);
	// Lets compare process perform SIGTERM cleanup without extending scan deadline.
	constexpr auto kCompareTerminationGrace = std::chrono::milliseconds(250);

	using howdy::native::CompareExit;

	auto MakeCompareWaitExitStatus(CompareExit exit_code) -> int {
		return howdy::native::EncodeWaitExitStatus(exit_code);
	}

	auto TryWaitForCompare(pid_t child_pid, int *status) -> bool {
		while (true) {
			const pid_t result = waitpid(child_pid, status, WNOHANG);
			if (result == child_pid) {
				return true;
			}
			if (result == 0) {
				return false;
			}
			if (errno == EINTR) {
				continue;
			}
			const int error_number = errno;
			syslog(LOG_ERR, "waitpid failed for compare process: %s (%d)",
			       howdy::native::SystemErrorMessage(error_number).c_str(), error_number);
			*status = MakeCompareWaitExitStatus(CompareExit::kAbort);
			return true;
		}
	}

	auto WaitForCompareUntil(pid_t child_pid, std::chrono::steady_clock::time_point deadline,
	                         void                                      *cancellation_context,
	                         howdy::pam::CompareCancellationRequestedFn cancellation_requested,
	                         bool *cancelled) -> std::optional<int> {
		using Clock = std::chrono::steady_clock;
		while (true) {
			int status = 0;
			if (TryWaitForCompare(child_pid, &status)) {
				return status;
			}
			if (cancellation_requested != nullptr && cancellation_requested(cancellation_context)) {
				*cancelled = true;
				return std::nullopt;
			}
			const auto now = Clock::now();
			if (now >= deadline) {
				return std::nullopt;
			}
			std::this_thread::sleep_for(
			    std::min(std::chrono::duration_cast<Clock::duration>(kCompareWaitPollInterval),
			             deadline - now));
		}
	}

	auto WaitForCompareProcess(pid_t child_pid, std::chrono::steady_clock::time_point deadline,
	                           void                                      *cancellation_context,
	                           howdy::pam::CompareCancellationRequestedFn cancellation_requested)
	    -> int {
		using Clock = std::chrono::steady_clock;

		bool cancelled = false;
		if (const auto status = WaitForCompareUntil(child_pid, deadline, cancellation_context,
		                                            cancellation_requested, &cancelled);
		    status.has_value()) {
			return *status;
		}

		const int terminal_status = cancelled
		                                ? MakeCompareWaitExitStatus(CompareExit::kAbort)
		                                : MakeCompareWaitExitStatus(CompareExit::kTimeoutReached);

		int status = 0;
		if (TryWaitForCompare(child_pid, &status)) {
			return terminal_status;
		}
		if (kill(child_pid, SIGTERM) != 0) {
			const int error_number = errno;
			if (error_number != ESRCH) {
				syslog(LOG_WARNING, "Failed to terminate compare process: %s (%d)",
				       howdy::native::SystemErrorMessage(error_number).c_str(), error_number);
			}
		}

		bool ignored_cancellation = false;
		if (WaitForCompareUntil(child_pid, Clock::now() + kCompareTerminationGrace, nullptr,
		                        nullptr, &ignored_cancellation)
		        .has_value()) {
			return terminal_status;
		}
		if (kill(child_pid, SIGKILL) != 0) {
			const int error_number = errno;
			if (error_number != ESRCH) {
				syslog(LOG_WARNING, "Failed to kill timed-out compare process: %s (%d)",
				       howdy::native::SystemErrorMessage(error_number).c_str(), error_number);
			}
		}

		while (true) {
			status             = 0;
			const pid_t result = waitpid(child_pid, &status, 0);
			if (result == child_pid) {
				return terminal_status;
			}
			if (result < 0 && errno == EINTR) {
				continue;
			}
			if (result < 0 && errno != ECHILD) {
				const int error_number = errno;
				syslog(LOG_ERR, "waitpid failed while reaping timed-out compare process: %s (%d)",
				       howdy::native::SystemErrorMessage(error_number).c_str(), error_number);
			}
			return terminal_status;
		}
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
		return WaitForCompareProcess(child_pid, deadline, nullptr, nullptr);
	}

	auto WaitUntil(pid_t child_pid, std::chrono::steady_clock::time_point deadline,
	               void                          *cancellation_context,
	               CompareCancellationRequestedFn cancellation_requested) -> int {
		return WaitForCompareProcess(child_pid, deadline, cancellation_context,
		                             cancellation_requested);
	}

	auto Wait(void *context, pid_t child_pid, std::chrono::steady_clock::time_point deadline,
	          void *cancellation_context, CompareCancellationRequestedFn cancellation_requested)
	    -> int {
		(void)context;
		return WaitUntil(child_pid, deadline, cancellation_context, cancellation_requested);
	}

	void CancelAndReap(pid_t child_pid) noexcept {
		try {
			(void)WaitForCompareProcess(child_pid, std::chrono::steady_clock::now(), nullptr,
			                            nullptr);
		} catch (...) {
			(void)kill(child_pid, SIGKILL);
			int status = 0;
			while (waitpid(child_pid, &status, 0) < 0 && errno == EINTR) {
			}
		}
	}

}  // namespace howdy::pam::compare_process
