#pragma once

namespace howdy::native {

	using HowdyDispatchFn = int (*)(int argc, char **argv);

	auto RunHowdyEntrypoint(int argc, char **argv, HowdyDispatchFn dispatch) -> int;

}  // namespace howdy::native
