#pragma once

#include "compare/sandbox.hpp"

#include <sys/resource.h>

namespace howdy::native::compare_sandbox_internal {

	using RlimitResource = decltype(RLIMIT_CPU);
	using GetrlimitFn    = int (*)(void *context, RlimitResource resource, rlimit *limit);
	using SetrlimitFn    = int (*)(void *context, RlimitResource resource, const rlimit *limit);
	using PrctlFn        = int (*)(void *context, int operation, unsigned long argument2,
	                               unsigned long argument3, unsigned long argument4,
	                               unsigned long argument5);

	struct CompareSandboxDependencies {
		void       *context   = nullptr;
		GetrlimitFn getrlimit = nullptr;
		SetrlimitFn setrlimit = nullptr;
		PrctlFn     prctl     = nullptr;
	};

	[[nodiscard]] auto apply_compare_sandbox(int                               timeout_seconds,
	                                         const CompareSandboxDependencies &dependencies)
	    -> CompareSandboxResult;

}  // namespace howdy::native::compare_sandbox_internal
