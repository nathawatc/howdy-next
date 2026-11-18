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
	// On success `child_pidfd` receives an owned pidfd for the child taken right after spawn, which
	// the caller passes to the wait. Both out-parameters are required (EINVAL otherwise). When no
	// pidfd can be taken, the child is stopped and Spawn returns the error.
	__attribute__((visibility("hidden"))) auto Spawn(const CompareLaunchRequest &request,
	                                                 pid_t *child_pid, int *child_pidfd,
	                                                 const Operations &operations) -> int;
	// `child_pidfd` is the borrowed pidfd from Spawn and is required: with -1 the child is
	// stopped by PID and the wait fails closed. cancellation_fd is borrowed until return; signal it
	// whenever cancellation becomes true. The callback preserves the initial cancellation/exit race
	// and is not polled.
	__attribute__((visibility("hidden"))) auto
	WaitUntil(pid_t child_pid, int child_pidfd, std::chrono::steady_clock::time_point deadline,
	          void *cancellation_context, CompareCancellationRequestedFn cancellation_requested,
	          int cancellation_fd) -> int;
	__attribute__((visibility("hidden"))) auto
	Wait(void *context, pid_t child_pid, int child_pidfd,
	     std::chrono::steady_clock::time_point deadline, void *cancellation_context,
	     CompareCancellationRequestedFn cancellation_requested, int cancellation_fd) -> int;
	__attribute__((visibility("hidden"))) void CancelAndReap(pid_t child_pid,
	                                                         int   child_pidfd) noexcept;

}  // namespace howdy::pam::compare_process
