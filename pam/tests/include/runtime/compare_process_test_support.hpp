#pragma once

#include "runtime/child_process.hpp"
#include "runtime/compare_launch.hpp"
#include "runtime/compare_process.hpp"
#include "support/scoped_fd.hpp"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <ranges>
#include <string>
#include <vector>

#include <spawn.h>
#include <unistd.h>

#include <sys/types.h>

namespace howdy::test::prompt_coordinator {

	struct PosixSpawnCapture {
		int                               init_calls          = 0;
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

	inline auto CapturePosixSpawnFileActionsInit(void *context, posix_spawn_file_actions_t *actions)
	    -> int {
		auto &capture = *static_cast<PosixSpawnCapture *>(context);
		++capture.init_calls;
		capture.initialized_actions = actions;
		return capture.init_result;
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
		    .file_actions_init         = CapturePosixSpawnFileActionsInit,
		    .file_actions_addclosefrom = CapturePosixSpawnFileActionsAddclosefrom,
		    .file_actions_destroy      = CapturePosixSpawnFileActionsDestroy,
		    .spawn                     = CapturePosixSpawn,
		};
	}

	// Waits for a child the test forked itself. Production takes the pidfd at spawn; a test may
	// open it here because it knows the child is still its own unreaped child.
	inline auto
	WaitUntilForTestChild(pid_t child_pid, std::chrono::steady_clock::time_point deadline,
	                      void                                      *cancellation_context,
	                      howdy::pam::CompareCancellationRequestedFn cancellation_requested,
	                      int                                        cancellation_fd) -> int {
		const howdy::native::ScopedFd pidfd(howdy::pam::runtime::OpenChildPidfd(child_pid));
		return howdy::pam::compare_process::WaitUntil(child_pid, pidfd.Get(), deadline,
		                                              cancellation_context, cancellation_requested,
		                                              cancellation_fd);
	}

}  // namespace howdy::test::prompt_coordinator
