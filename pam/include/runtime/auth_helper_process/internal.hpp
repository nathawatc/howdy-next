#pragma once

// clang-format off
// Declare pid_t from <sys/types.h> before <chrono> can declare it in <time.h>.
#include <sys/types.h>
// clang-format on

#include <chrono>
#include <cstdint>

namespace howdy::pam::auth_helper_process::internal {

	using HelperDeadline = std::chrono::steady_clock::time_point;

	enum class HelperWaitResult : std::uint8_t {
		kExited,
		kTimedOut,
		// ECHILD: the helper is no longer a child this process can wait for, normally because the
		// PAM application reaped it and took its exit status.
		kReapedElsewhere,
		kWaitError,
	};

	__attribute__((visibility("hidden"))) auto DeadlinePollTimeout(HelperDeadline deadline) -> int;
	// `pidfd` is the borrowed descriptor taken at spawn and must be passed explicitly. With -1
	// every operation uses the PID, each only after the kernel reports an unreaped child with it;
	// production always passes one, and only tests use -1.
	__attribute__((visibility("hidden"))) void TerminateAndReapHelperProcess(pid_t child_pid,
	                                                                         int   pidfd) noexcept;
	__attribute__((visibility("hidden"))) auto
	WaitForHelperProcessUntil(pid_t child_pid, HelperDeadline deadline, int *status,
	                          int pidfd) noexcept -> HelperWaitResult;

}  // namespace howdy::pam::auth_helper_process::internal
