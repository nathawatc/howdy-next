#pragma once

#include "runtime/child_process.hpp"

#include <spawn.h>

#include <sys/socket.h>

namespace howdy::pam::runtime::internal {

	// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
	__attribute__((visibility("hidden"))) inline auto
	Socketpair(void *context, int domain, int type, int protocol, int *socket_fds) -> int {
		(void)context;
		return socketpair(domain, type, protocol, socket_fds);
	}

	__attribute__((visibility("hidden"))) inline auto
	ActionsInit(void *context, posix_spawn_file_actions_t *actions) -> int {
		(void)context;
		return posix_spawn_file_actions_init(actions);
	}

	__attribute__((visibility("hidden"))) inline auto
	ActionsAdddup2(void *context, posix_spawn_file_actions_t *actions, int source_fd, int target_fd)
	    -> int {
		(void)context;
		return posix_spawn_file_actions_adddup2(actions, source_fd, target_fd);
	}

	__attribute__((visibility("hidden"))) inline auto
	ActionsAddclosefrom(void *context, posix_spawn_file_actions_t *actions, int from_fd) -> int {
		(void)context;
		return posix_spawn_file_actions_addclosefrom_np(actions, from_fd);
	}

	__attribute__((visibility("hidden"))) inline auto
	ActionsDestroy(void *context, posix_spawn_file_actions_t *actions) -> int {
		(void)context;
		return posix_spawn_file_actions_destroy(actions);
	}

	__attribute__((visibility("hidden"))) inline auto Spawn(const SpawnRequest &request) -> int {
		(void)request.context;
		return SpawnChild(request.child_pid, request.child_pidfd,
		                  {
		                      .path    = request.path,
		                      .actions = request.actions,
		                      .argv    = request.argv.data(),
		                      .envp    = request.envp.data(),
		                  });
	}

}  // namespace howdy::pam::runtime::internal
