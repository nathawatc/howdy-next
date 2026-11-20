#pragma once

#include "protocol/compare_verdict.hpp"
#include "protocol/inherited_fd.hpp"

namespace howdy::native::compare_descriptors {

	// Descriptor mode: instead of paths, PAM hands compare the runtime config at kConfigFd and the
	// user's model at kModelFd, each opened read-only from the root-owned store, and announces
	// them with kConfigEnvironmentEntry and kModelEnvironmentEntry. Both come together and only
	// with the verdict socket (protocol/compare_verdict.hpp); compare rejects any other mix, and a
	// --config argument alongside them. Compare owns the two descriptors, loads them before it
	// opens anything else, and closes each one right after loading it. Without the two variables
	// compare runs in path mode, which a PAM module loaded before the upgrade still uses.
	inline constexpr int  kConfigFd               = 4;
	inline constexpr auto kConfigEnvironmentName  = "HOWDY_COMPARE_CONFIG_FD";
	inline constexpr auto kConfigEnvironmentValue = "4";
	inline constexpr auto kConfigEnvironmentEntry = "HOWDY_COMPARE_CONFIG_FD=4";

	inline constexpr int  kModelFd               = 5;
	inline constexpr auto kModelEnvironmentName  = "HOWDY_COMPARE_MODEL_FD";
	inline constexpr auto kModelEnvironmentValue = "5";
	inline constexpr auto kModelEnvironmentEntry = "HOWDY_COMPARE_MODEL_FD=5";

	// The lowest descriptor PAM closes in compare; everything from here up is inherited by mistake.
	inline constexpr int kFirstUnusedFd = kModelFd + 1;

	static_assert(InheritedFdEntryMatches(kConfigFd, kConfigEnvironmentName,
	                                      kConfigEnvironmentValue, kConfigEnvironmentEntry));
	static_assert(InheritedFdEntryMatches(kModelFd, kModelEnvironmentName, kModelEnvironmentValue,
	                                      kModelEnvironmentEntry));
	// The descriptors PAM passes are contiguous from the verdict socket up, so a closefrom of
	// kFirstUnusedFd leaves no inherited gap below them.
	static_assert(kConfigFd == compare_verdict::kFd + 1 && kModelFd == kConfigFd + 1);

}  // namespace howdy::native::compare_descriptors
