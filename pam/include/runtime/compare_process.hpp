#pragma once

#include "prompt/prompt_coordinator.hpp"

#include <chrono>

#include <sys/types.h>

namespace howdy::pam::compare_process {

	__attribute__((visibility("hidden"))) auto
	spawn(void *context, const CompareLaunchRequest &request, pid_t *child_pid) -> int;
	__attribute__((visibility("hidden"))) auto wait(void *context, pid_t child_pid,
	                                                std::chrono::steady_clock::time_point deadline)
	    -> int;
	__attribute__((visibility("hidden"))) auto terminate(void *context, pid_t child_pid) -> void;

}  // namespace howdy::pam::compare_process
