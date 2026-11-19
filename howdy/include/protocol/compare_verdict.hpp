#pragma once

#include <string_view>

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

	// The strings spell kFd out by hand; keep them in step with it.
	static_assert(kFd >= 0 && kFd <= 9 && std::string_view(kEnvironmentValue).size() == 1 &&
	              std::string_view(kEnvironmentValue).front() == '0' + kFd);
	static_assert(
	    std::string_view(kEnvironmentEntry).starts_with(kEnvironmentName) &&
	    std::string_view(kEnvironmentEntry).ends_with(kEnvironmentValue) &&
	    std::string_view(kEnvironmentEntry).size() ==
	        std::string_view(kEnvironmentName).size() + 1 +
	            std::string_view(kEnvironmentValue).size() &&
	    std::string_view(kEnvironmentEntry).at(std::string_view(kEnvironmentName).size()) == '=');

}  // namespace howdy::native::compare_verdict
