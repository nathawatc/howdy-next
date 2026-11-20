#pragma once

#include "protocol/inherited_fd.hpp"

namespace howdy::native::compare_verdict {

	// PAM hands compare one end of a private unix socket pair at kFd and sets kEnvironmentEntry.
	// Right before it exits, compare writes its exit code there as one byte. PAM reads that byte
	// only when the PAM application reaped compare and took the exit status with it (for example
	// a host with SIGCHLD set to SIG_IGN), and accepts it only as the one byte the socket held;
	// otherwise the exit status decides. A socket, unlike a pipe, cannot be reopened through
	// /proc/<pid>/fd, so another process cannot add a byte of its own.
	inline constexpr int  kFd               = 3;
	inline constexpr auto kEnvironmentName  = "HOWDY_COMPARE_VERDICT_FD";
	inline constexpr auto kEnvironmentValue = "3";
	inline constexpr auto kEnvironmentEntry = "HOWDY_COMPARE_VERDICT_FD=3";

	static_assert(InheritedFdEntryMatches(kFd, kEnvironmentName, kEnvironmentValue,
	                                      kEnvironmentEntry));

}  // namespace howdy::native::compare_verdict
