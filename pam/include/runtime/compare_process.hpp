#pragma once

#include "runtime/child_process.hpp"
#include "runtime/compare_launch.hpp"

#include <chrono>

#include <spawn.h>

#include <sys/types.h>

namespace howdy::pam::compare_process {

	using SpawnRequest = runtime::SpawnRequest;

	struct Operations {
		void *context                                                                  = nullptr;
		int (*socketpair)(void *, int domain, int type, int protocol, int *socket_fds) = nullptr;
		int (*file_actions_init)(void *, posix_spawn_file_actions_t *)                 = nullptr;
		int (*file_actions_adddup2)(void *, posix_spawn_file_actions_t *, int fd,
		                            int new_fd)                                        = nullptr;
		int (*file_actions_addclosefrom)(void *, posix_spawn_file_actions_t *,
		                                 int from_fd)                                  = nullptr;
		int (*file_actions_destroy)(void *, posix_spawn_file_actions_t *)              = nullptr;
		int (*spawn)(const SpawnRequest &)                                             = nullptr;
	};

	__attribute__((visibility("hidden"))) auto ProductionOperations() -> Operations;
	// Spawns compare in descriptor mode: the verdict socket at fd 3, the request's config and
	// model at fds 4 and 5 (protocol/compare_descriptors.hpp), nothing above. All three are
	// required, so a socket or file action that cannot be set up fails the spawn. On success
	// `child_pidfd` receives an owned pidfd for the child taken right after spawn, and
	// `verdict_fd` the owned, non-blocking PAM end of the verdict socket compare reports its exit
	// code on (protocol/compare_verdict.hpp); the caller passes both to the wait. All
	// out-parameters, the username and both descriptors are required (EINVAL otherwise). When no
	// pidfd can be taken, the child is stopped and Spawn returns the error.
	__attribute__((visibility("hidden"))) auto Spawn(const CompareLaunchRequest &request,
	                                                 pid_t *child_pid, int *child_pidfd,
	                                                 int *verdict_fd, const Operations &operations)
	    -> int;
	// `child_pidfd` is the borrowed pidfd from Spawn and is required: with -1 the child is
	// stopped by PID and the wait fails closed. `verdict_fd` is the borrowed verdict socket from
	// Spawn, or -1 for none; it is read only when the PAM application reaped compare and took
	// the exit status, and without exactly one verdict byte that wait fails closed. A timeout or
	// cancellation stays one when compare is gone by the time this wait ends it, whether this
	// wait or the application reaped it.
	// cancellation_fd is borrowed until return; signal it whenever cancellation becomes true. The
	// callback preserves the initial cancellation/exit race and is not polled.
	__attribute__((visibility("hidden"))) auto
	WaitUntil(pid_t child_pid, int child_pidfd, int verdict_fd,
	          std::chrono::steady_clock::time_point deadline, void *cancellation_context,
	          CompareCancellationRequestedFn cancellation_requested, int cancellation_fd) -> int;
	__attribute__((visibility("hidden"))) auto
	Wait(void *context, pid_t child_pid, int child_pidfd, int verdict_fd,
	     std::chrono::steady_clock::time_point deadline, void *cancellation_context,
	     CompareCancellationRequestedFn cancellation_requested, int cancellation_fd) -> int;
	__attribute__((visibility("hidden"))) void CancelAndReap(pid_t child_pid,
	                                                         int   child_pidfd) noexcept;

}  // namespace howdy::pam::compare_process
