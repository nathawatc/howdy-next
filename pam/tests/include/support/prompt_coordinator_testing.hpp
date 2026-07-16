#pragma once

#ifdef HOWDY_PAM_TESTING

#	include "prompt/native_prompt_conversation.hpp"
#	include "prompt/optional_task.hpp"
#	include "prompt/prompt_coordinator.hpp"

#	include <chrono>
#	include <spawn.h>
#	include <tuple>

namespace howdy::pam::testing {
	using PosixSpawnFileActionsInitFn = int (*)(void *context, posix_spawn_file_actions_t *actions);
	using PosixSpawnFileActionsAddCloseFromFn = int (*)(void                       *context,
	                                                    posix_spawn_file_actions_t *actions,
	                                                    int                         from_fd);
	using PosixSpawnFileActionsDestroyFn      = int (*)(void                       *context,
	                                                    posix_spawn_file_actions_t *actions);

	struct PosixSpawnRequest {
		void                             *context   = nullptr;
		pid_t                            *child_pid = nullptr;
		const char                       *path      = nullptr;
		const posix_spawn_file_actions_t *actions   = nullptr;
		char *const                      *argv      = nullptr;
		char *const                      *envp      = nullptr;
	};

	using PosixSpawnFn = int (*)(const PosixSpawnRequest &request);

	struct PosixSpawnOperations {
		PosixSpawnFileActionsInitFn         file_actions_init         = nullptr;
		PosixSpawnFileActionsAddCloseFromFn file_actions_addclosefrom = nullptr;
		PosixSpawnFileActionsDestroyFn      file_actions_destroy      = nullptr;
		PosixSpawnFn                        spawn                     = nullptr;
	};

	void cleanup_native_prompt(optional_task<std::tuple<int, char *>> &pass_task,
	                           NativePromptConversation               &native_prompt) noexcept;

	auto spawn_compare_process(const CompareLaunchRequest &request, pid_t *child_pid,
	                           const PosixSpawnOperations &operations, void *context) -> int;
	auto wait_for_compare_process(pid_t child_pid, std::chrono::steady_clock::duration hard_timeout)
	    -> int;

}  // namespace howdy::pam::testing

#endif
