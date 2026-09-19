#pragma once

#include "compare/sandbox.hpp"
#include "support/prctl_types.hpp"

#include <sys/resource.h>

namespace howdy::native::compare_sandbox_internal {

	using RlimitResource = decltype(RLIMIT_CPU);
	using GetrlimitFn    = int (*)(void *context, RlimitResource resource, rlimit *limit);
	using SetrlimitFn    = int (*)(void *context, RlimitResource resource, const rlimit *limit);
	using PrctlFn        = int (*)(void *context, int operation, PrctlArgument argument2,
	                               PrctlArgument argument3, PrctlArgument argument4,
	                               PrctlArgument argument5);

	struct CompareSandboxDependencies {
		void       *context   = nullptr;
		GetrlimitFn getrlimit = nullptr;
		SetrlimitFn setrlimit = nullptr;
		PrctlFn     prctl     = nullptr;
	};

	[[nodiscard]] auto ApplyCompareSandbox(int                               timeout_seconds,
	                                       const CompareSandboxDependencies &dependencies)
	    -> CompareSandboxResult;

}  // namespace howdy::native::compare_sandbox_internal
