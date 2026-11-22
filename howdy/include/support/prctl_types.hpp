#pragma once

#include <sys/prctl.h>

namespace howdy::native {

	// prctl(2) is variadic and its optional arguments use the native unsigned-long ABI width.
	using PrctlArgument = decltype(0UL);

	using PrctlFn = int (*)(void *context, int operation, PrctlArgument argument2,
	                        PrctlArgument argument3, PrctlArgument argument4,
	                        PrctlArgument argument5);

	inline auto SystemPrctl([[maybe_unused]] void *context, int operation, PrctlArgument argument2,
	                        PrctlArgument argument3, PrctlArgument argument4,
	                        PrctlArgument argument5) -> int {
		return prctl(operation, argument2, argument3, argument4, argument5);
	}

}  // namespace howdy::native
