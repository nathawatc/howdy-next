#pragma once

#include "runtime/compare_launch.hpp"

#include <chrono>
#include <span>

#include <spawn.h>

#include <sys/types.h>

namespace howdy::pam::compare_process {

	struct SpawnRequest {
		void                             *context   = nullptr;
		pid_t                            *child_pid = nullptr;
		const char                       *path      = nullptr;
		const posix_spawn_file_actions_t *actions   = nullptr;
		std::span<char *const>            argv;
		std::span<char *const>            envp;
	};

	struct Operations {
		void *context                                                     = nullptr;
		int (*file_actions_init)(void *, posix_spawn_file_actions_t *)    = nullptr;
		int (*file_actions_addclosefrom)(void *, posix_spawn_file_actions_t *,
		                                 int from_fd)                     = nullptr;
		int (*file_actions_destroy)(void *, posix_spawn_file_actions_t *) = nullptr;
		int (*spawn)(const SpawnRequest &)                                = nullptr;
	};

	__attribute__((visibility("hidden"))) auto ProductionOperations() -> Operations;
	__attribute__((visibility("hidden"))) auto Spawn(const CompareLaunchRequest &request,
	                                                 pid_t *child_pid, const Operations &operations)
	    -> int;
	// cancellation_fd is borrowed until return; signal it whenever cancellation becomes true.
	// The callback preserves the initial cancellation/exit race and is not polled.
	__attribute__((visibility("hidden"))) auto
	WaitUntil(pid_t child_pid, std::chrono::steady_clock::time_point deadline,
	          void *cancellation_context, CompareCancellationRequestedFn cancellation_requested,
	          int cancellation_fd) -> int;
	__attribute__((visibility("hidden"))) auto
	Wait(void *context, pid_t child_pid, std::chrono::steady_clock::time_point deadline,
	     void *cancellation_context, CompareCancellationRequestedFn cancellation_requested,
	     int cancellation_fd) -> int;
	__attribute__((visibility("hidden"))) void CancelAndReap(pid_t child_pid) noexcept;

}  // namespace howdy::pam::compare_process
