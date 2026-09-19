#pragma once

namespace howdy::native {

	// prctl(2) is variadic and its optional arguments use the native unsigned-long ABI width.
	using PrctlArgument = decltype(0UL);

}  // namespace howdy::native
