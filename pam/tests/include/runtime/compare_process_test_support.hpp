#pragma once

#include "protocol/compare_verdict.hpp"
#include "runtime/child_process.hpp"
#include "runtime/compare_launch.hpp"
#include "runtime/compare_process.hpp"
#include "support/scoped_fd.hpp"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <optional>
#include <ranges>
#include <string>
#include <vector>

#include <fcntl.h>
#include <spawn.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/types.h>

namespace howdy::test::prompt_coordinator {

	struct Dup2Action {
		int fd     = -1;
		int new_fd = -1;
	};

	struct PosixSpawnCapture {
		int init_calls    = 0;
		int adddup2_calls = 0;
		// Every dup2 action in order, as Spawn added them.
		std::vector<Dup2Action> adddup2;
		// adddup2_result is returned by the call with this index (0-based), or by every call when
		// it is -1.
		int adddup2_fail_index = -1;
		int adddup2_result     = 0;
		int socketpair_error   = 0;
		// Read end of a gate a spawn callback's child may wait on, or -1.
		int                               spawn_gate_fd       = -1;
		int                               addclosefrom_calls  = 0;
		int                               destroy_calls       = 0;
		int                               spawn_calls         = 0;
		int                               init_result         = 0;
		int                               addclosefrom_result = 0;
		int                               spawn_result        = 0;
		int                               closefrom_fd        = -1;
		pid_t                             next_pid            = 4242;
		posix_spawn_file_actions_t       *initialized_actions = nullptr;
		posix_spawn_file_actions_t       *closefrom_actions   = nullptr;
		posix_spawn_file_actions_t       *destroyed_actions   = nullptr;
		const posix_spawn_file_actions_t *spawn_actions       = nullptr;
		std::string                       path;
		std::vector<std::string>          argv;
		std::vector<std::string>          environment;
	};

	// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
	inline auto CaptureSocketpair(void *context, int domain, int type, int protocol,
	                              int *socket_fds) -> int {
		const auto &capture = *static_cast<const PosixSpawnCapture *>(context);
		if (capture.socketpair_error != 0) {
			errno = capture.socketpair_error;
			return -1;
		}
		return socketpair(domain, type, protocol, socket_fds);
	}

	inline auto CapturePosixSpawnFileActionsInit(void *context, posix_spawn_file_actions_t *actions)
	    -> int {
		auto &capture = *static_cast<PosixSpawnCapture *>(context);
		++capture.init_calls;
		capture.initialized_actions = actions;
		return capture.init_result;
	}

	// NOLINTBEGIN(bugprone-easily-swappable-parameters)
	inline auto CapturePosixSpawnFileActionsAdddup2(void                       *context,
	                                                posix_spawn_file_actions_t *actions, int fd,
	                                                int new_fd) -> int {
		// NOLINTEND(bugprone-easily-swappable-parameters)
		(void)actions;
		auto     &capture = *static_cast<PosixSpawnCapture *>(context);
		const int index   = capture.adddup2_calls++;
		capture.adddup2.push_back({.fd = fd, .new_fd = new_fd});
		return capture.adddup2_fail_index < 0 || capture.adddup2_fail_index == index
		           ? capture.adddup2_result
				   : 0;
	}

	inline auto CapturePosixSpawnFileActionsAddclosefrom(void                       *context,
	                                                     posix_spawn_file_actions_t *actions,
	                                                     int from_fd) -> int {
		auto &capture = *static_cast<PosixSpawnCapture *>(context);
		++capture.addclosefrom_calls;
		capture.closefrom_actions = actions;
		capture.closefrom_fd      = from_fd;
		return capture.addclosefrom_result;
	}

	inline auto CapturePosixSpawnFileActionsDestroy(void                       *context,
	                                                posix_spawn_file_actions_t *actions) -> int {
		auto &capture = *static_cast<PosixSpawnCapture *>(context);
		++capture.destroy_calls;
		capture.destroyed_actions = actions;
		return 0;
	}

	inline auto CapturePosixSpawn(const howdy::pam::compare_process::SpawnRequest &request) -> int {
		auto &capture = *static_cast<PosixSpawnCapture *>(request.context);
		++capture.spawn_calls;
		capture.spawn_actions = request.actions;
		capture.path          = request.path;
		for (const char *argument :
		     request.argv | std::views::take_while([](const char *value) -> bool {
			     return value != nullptr;
		     })) {
			capture.argv.emplace_back(argument);
		}
		for (const char *entry :
		     request.envp | std::views::take_while([](const char *value) -> bool {
			     return value != nullptr;
		     })) {
			capture.environment.emplace_back(entry);
		}

		if (capture.spawn_result != 0) {
			return capture.spawn_result;
		}
		// A real, immediately exiting child, so Spawn can take a pidfd for it; the test reaps it.
		const pid_t child_pid = fork();
		if (child_pid < 0) {
			return errno;
		}
		if (child_pid == 0) {
			_exit(EXIT_SUCCESS);
		}
		capture.next_pid   = child_pid;
		*request.child_pid = child_pid;
		return 0;
	}

	inline auto PosixSpawnOperations(void *context) -> howdy::pam::compare_process::Operations {
		return {
		    .context                   = context,
		    .socketpair                = CaptureSocketpair,
		    .file_actions_init         = CapturePosixSpawnFileActionsInit,
		    .file_actions_adddup2      = CapturePosixSpawnFileActionsAdddup2,
		    .file_actions_addclosefrom = CapturePosixSpawnFileActionsAddclosefrom,
		    .file_actions_destroy      = CapturePosixSpawnFileActionsDestroy,
		    .spawn                     = CapturePosixSpawn,
		};
	}

	// The source Spawn placed at `new_fd` in compare, if it added that action.
	inline auto Dup2SourceFor(const PosixSpawnCapture &capture, int new_fd) -> std::optional<int> {
		for (const auto &action : capture.adddup2) {
			if (action.new_fd == new_fd) {
				return action.fd;
			}
		}
		return std::nullopt;
	}

	// Real descriptors for a compare request, which the production Spawn copies before placing.
	class CompareRequestFiles {
	public:
		[[nodiscard]] auto Request(const std::string &username = "alice") const
		    -> howdy::pam::CompareLaunchRequest {
			return {.username = username, .config_fd = config_.Get(), .model_fd = model_.Get()};
		}

		[[nodiscard]] auto ConfigFd() const -> int {
			return config_.Get();
		}

		[[nodiscard]] auto ModelFd() const -> int {
			return model_.Get();
		}

	private:
		howdy::native::ScopedFd config_{open("/dev/null", O_RDONLY | O_CLOEXEC)};
		howdy::native::ScopedFd model_{open("/dev/null", O_RDONLY | O_CLOEXEC)};
	};

	// Waits for a child the test forked itself. Production takes the pidfd at spawn; a test may
	// open it here because it knows the child is still its own unreaped child.
	inline auto
	WaitUntilForTestChild(pid_t child_pid, std::chrono::steady_clock::time_point deadline,
	                      void                                      *cancellation_context,
	                      howdy::pam::CompareCancellationRequestedFn cancellation_requested,
	                      int                                        cancellation_fd) -> int {
		const howdy::native::ScopedFd pidfd(howdy::pam::runtime::OpenChildPidfd(child_pid));
		return howdy::pam::compare_process::WaitUntil(child_pid, pidfd.Get(), -1, deadline,
		                                              cancellation_context, cancellation_requested,
		                                              cancellation_fd);
	}

}  // namespace howdy::test::prompt_coordinator
