#pragma once

#include "runtime/compare_process.hpp"

#include <spawn.h>
#include <string>
#include <vector>

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
		for (char *const *argument = request.argv; *argument != nullptr; ++argument) {
			capture.argv.emplace_back(*argument);
		}
		for (char *const *entry = request.envp; *entry != nullptr; ++entry) {
			capture.environment.emplace_back(*entry);
		}

		if (capture.spawn_result == 0) {
			*request.child_pid = capture.next_pid;
		}
		return capture.spawn_result;
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

}  // namespace howdy::test::prompt_coordinator
