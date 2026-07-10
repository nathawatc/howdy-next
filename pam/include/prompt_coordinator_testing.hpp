#pragma once

#ifdef HOWDY_PAM_TESTING

#	include "native_prompt_conversation.hpp"
#	include "optional_task.hpp"
#	include "prompt_coordinator.hpp"

#	include <tuple>
#	include <chrono>

namespace howdy::pam::testing {
	using PosixSpawnFn = int (*)(void *context, pid_t *child_pid, const char *path,
	                             char *const *argv, char *const *envp);

	void cleanup_native_prompt(optional_task<std::tuple<int, char *>> &pass_task,
	                           NativePromptConversation               &native_prompt) noexcept;

	auto spawn_compare_process(const CompareLaunchRequest &request, pid_t *child_pid,
	                           PosixSpawnFn posix_spawn_fn, void *context) -> int;
	auto wait_for_compare_process(pid_t child_pid, std::chrono::steady_clock::duration hard_timeout)
	    -> int;

}  // namespace howdy::pam::testing

#endif
