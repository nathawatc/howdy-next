#pragma once

#include "runtime/compare_launch.hpp"

#include <chrono>
#include <spawn.h>

#include <sys/types.h>

namespace howdy::pam::compare_process {

	struct SpawnRequest {
		void                             *context   = nullptr;
		pid_t                            *child_pid = nullptr;
		const char                       *path      = nullptr;
		const posix_spawn_file_actions_t *actions   = nullptr;
		char *const                      *argv      = nullptr;
		char *const                      *envp      = nullptr;
	};

	struct Operations {
		void *context                                                     = nullptr;
		int (*file_actions_init)(void *, posix_spawn_file_actions_t *)    = nullptr;
		int (*file_actions_addclosefrom)(void *, posix_spawn_file_actions_t *,
		                                 int from_fd)                     = nullptr;
		int (*file_actions_destroy)(void *, posix_spawn_file_actions_t *) = nullptr;
		int (*spawn)(const SpawnRequest &)                                = nullptr;
	};

	__attribute__((visibility("hidden"))) auto production_operations() -> Operations;
	__attribute__((visibility("hidden"))) auto spawn(const CompareLaunchRequest &request,
	                                                 pid_t *child_pid, const Operations &operations)
	    -> int;
	__attribute__((visibility("hidden"))) auto
	wait_until(pid_t child_pid, std::chrono::steady_clock::time_point deadline) -> int;
	__attribute__((visibility("hidden"))) auto
	wait_until(pid_t child_pid, std::chrono::steady_clock::time_point deadline,
	           void *cancellation_context, CompareCancellationRequestedFn cancellation_requested)
	    -> int;
	__attribute__((visibility("hidden"))) auto
	spawn(void *context, const CompareLaunchRequest &request, pid_t *child_pid) -> int;
	__attribute__((visibility("hidden"))) auto
	wait(void *context, pid_t child_pid, std::chrono::steady_clock::time_point deadline,
	     void *cancellation_context, CompareCancellationRequestedFn cancellation_requested) -> int;
	__attribute__((visibility("hidden"))) void cancel_and_reap(pid_t child_pid) noexcept;

}  // namespace howdy::pam::compare_process
