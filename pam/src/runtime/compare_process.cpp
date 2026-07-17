#ifndef _GNU_SOURCE
#	define _GNU_SOURCE
#endif

#include "runtime/compare_process.hpp"

#include "paths.hpp"
#include "protocol/compare_exit.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <optional>
#include <spawn.h>
#include <string>
#include <syslog.h>
#include <thread>

#include <sys/wait.h>

namespace {
	using howdy::pam::compare_process::Operations;
	using howdy::pam::compare_process::SpawnRequest;

	constexpr auto kCompareWaitPollInterval = std::chrono::milliseconds(10);
	// Lets compare process perform SIGTERM cleanup without extending scan deadline.
	constexpr auto kCompareTerminationGrace = std::chrono::milliseconds(250);

	using howdy::native::CompareExit;

	auto make_wait_exit_status(CompareExit exit_code) -> int {
		return static_cast<int>(exit_code) << 8;
	}

	auto try_wait_for_compare(pid_t child_pid, int *status) -> bool {
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
			syslog(LOG_ERR, "waitpid failed for compare process: %s (%d)", strerror(errno), errno);
			*status = make_wait_exit_status(CompareExit::kAbort);
			return true;
		}
	}

	auto wait_for_compare_until(pid_t child_pid, std::chrono::steady_clock::time_point deadline)
	    -> std::optional<int> {
		using Clock = std::chrono::steady_clock;
		while (true) {
			int status = 0;
			if (try_wait_for_compare(child_pid, &status)) {
				return status;
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

	auto wait_for_compare_process(pid_t child_pid, std::chrono::steady_clock::time_point deadline)
	    -> int {
		using Clock = std::chrono::steady_clock;

		if (const auto status = wait_for_compare_until(child_pid, deadline); status.has_value()) {
			return *status;
		}

		int status = 0;
		if (try_wait_for_compare(child_pid, &status)) {
			return status;
		}
		if (kill(child_pid, SIGTERM) != 0 && errno != ESRCH) {
			syslog(LOG_WARNING, "Failed to terminate timed-out compare process: %s (%d)",
			       strerror(errno), errno);
		}

		if (wait_for_compare_until(child_pid, Clock::now() + kCompareTerminationGrace)
		        .has_value()) {
			return make_wait_exit_status(CompareExit::kTimeoutReached);
		}
		if (kill(child_pid, SIGKILL) != 0 && errno != ESRCH) {
			syslog(LOG_WARNING, "Failed to kill timed-out compare process: %s (%d)",
			       strerror(errno), errno);
		}

		while (true) {
			status             = 0;
			const pid_t result = waitpid(child_pid, &status, 0);
			if (result == child_pid) {
				return make_wait_exit_status(CompareExit::kTimeoutReached);
			}
			if (result < 0 && errno == EINTR) {
				continue;
			}
			if (result < 0 && errno != ECHILD) {
				syslog(LOG_ERR, "waitpid failed while reaping timed-out compare process: %s (%d)",
				       strerror(errno), errno);
			}
			return make_wait_exit_status(CompareExit::kTimeoutReached);
		}
	}

	auto call_posix_spawn_file_actions_init(void *context, posix_spawn_file_actions_t *actions)
	    -> int {
		(void)context;
		return posix_spawn_file_actions_init(actions);
	}

	auto call_posix_spawn_file_actions_addclosefrom(void                       *context,
	                                                posix_spawn_file_actions_t *actions,
	                                                int                         from_fd) -> int {
		(void)context;
		return posix_spawn_file_actions_addclosefrom_np(actions, from_fd);
	}

	auto call_posix_spawn_file_actions_destroy(void *context, posix_spawn_file_actions_t *actions)
	    -> int {
		(void)context;
		return posix_spawn_file_actions_destroy(actions);
	}

	auto call_posix_spawn(const SpawnRequest &request) -> int {
		(void)request.context;
		return posix_spawn(request.child_pid, request.path, request.actions, nullptr, request.argv,
		                   request.envp);
	}

	constexpr Operations kPosixSpawnOperations = {
	    .context                   = nullptr,
	    .file_actions_init         = call_posix_spawn_file_actions_init,
	    .file_actions_addclosefrom = call_posix_spawn_file_actions_addclosefrom,
	    .file_actions_destroy      = call_posix_spawn_file_actions_destroy,
	    .spawn                     = call_posix_spawn,
	};

	auto spawn_compare_process(const howdy::pam::CompareLaunchRequest &request, pid_t *child_pid,
	                           const Operations &operations) -> int {
		const std::string config_path(request.config_path);
		const std::string username(request.username);

		std::array<char *, 5> args = {
		    const_cast<char *>(kCompareProcessPath),
		    const_cast<char *>("--config"),
		    const_cast<char *>(config_path.data()),
		    const_cast<char *>(username.data()),
		    nullptr,
		};

		const std::string user_models_env =
		    "HOWDY_USER_MODELS_DIR=" + std::string(request.user_models_dir);

		std::array<char *, 2> runtime_env = {
		    const_cast<char *>(user_models_env.data()),
		    nullptr,
		};
		std::array<char *, 1> empty_env = {
		    nullptr,
		};

		char **compare_env = request.staged_runtime ? runtime_env.data() : empty_env.data();

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
		                                           .argv      = args.data(),
		                                           .envp      = compare_env});
		(void)operations.file_actions_destroy(operations.context, &file_actions);
		return spawn_result;
	}

}  // namespace

namespace howdy::pam::compare_process {

	auto production_operations() -> Operations {
		return kPosixSpawnOperations;
	}

	auto spawn(const CompareLaunchRequest &request, pid_t *child_pid, const Operations &operations)
	    -> int {
		return spawn_compare_process(request, child_pid, operations);
	}

	auto wait_until(pid_t child_pid, std::chrono::steady_clock::time_point deadline) -> int {
		return wait_for_compare_process(child_pid, deadline);
	}

	auto spawn(void *context, const CompareLaunchRequest &request, pid_t *child_pid) -> int {
		(void)context;
		return spawn(request, child_pid, production_operations());
	}

	auto wait(void *context, pid_t child_pid, std::chrono::steady_clock::time_point deadline)
	    -> int {
		(void)context;
		return wait_until(child_pid, deadline);
	}

	auto terminate(void *context, pid_t child_pid) -> void {
		(void)context;
		if (kill(child_pid, SIGTERM) != 0 && errno != ESRCH) {
			syslog(LOG_WARNING, "Failed to terminate compare process: %s (%d)", strerror(errno),
			       errno);
		}
	}

}  // namespace howdy::pam::compare_process
