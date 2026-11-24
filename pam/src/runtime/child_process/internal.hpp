#pragma once

#include "runtime/child_process.hpp"
#include "support/scoped_fd.hpp"

#include <cerrno>
#include <chrono>
#include <limits>

#include <fcntl.h>
#include <spawn.h>

#include <sys/socket.h>

namespace howdy::pam::runtime::internal {

	// Clamps poll timeouts to [0, INT_MAX] so distant deadlines (like time_point::max()) never
	// wrap negative and make poll(2) block indefinitely.
	[[nodiscard]] __attribute__((visibility("hidden"))) inline auto PollTimeoutMs(
	    std::chrono::steady_clock::time_point deadline,
	    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) noexcept
	    -> int {
		if (deadline <= now) {
			return 0;
		}
		const auto     remaining = deadline - now;
		constexpr auto max_duration =
		    std::chrono::duration_cast<std::chrono::steady_clock::duration>(
		        std::chrono::milliseconds(std::numeric_limits<int>::max()));
		if (remaining >= max_duration) {
			return std::numeric_limits<int>::max();
		}
		return static_cast<int>(std::chrono::ceil<std::chrono::milliseconds>(remaining).count());
	}

	// Moves an owned descriptor to at least `min_fd` (preserving O_CLOEXEC semantics). On failure,
	// remaps EINVAL (RLIMIT_NOFILE below `min_fd`) to EMFILE so callers do not misinterpret a limit
	// failure as PidfdErrorMeansChildGone, leaves `*fd` open so the caller can still clean up via
	// it, and returns false.
	[[nodiscard]] __attribute__((visibility("hidden"))) inline auto
	RaiseFdAbove(howdy::native::ScopedFd *fd, int min_fd) noexcept -> bool {
		if (fd == nullptr || !fd->Valid()) {
			errno = EBADF;
			return false;
		}
		if (fd->Get() >= min_fd) {
			return true;
		}
		const int raised = fcntl(fd->Get(), F_DUPFD_CLOEXEC, min_fd);
		if (raised < 0) {
			errno = errno == EINVAL ? EMFILE : errno;
			return false;
		}
		fd->Reset(raised);
		return true;
	}

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
