#include "app/howdy_entrypoint.hpp"

#include "config/runtime_paths.hpp"

#include <exception>
#include <iostream>

#include <stdlib.h>

namespace howdy::native {

	auto RunHowdyEntrypoint(int argc, char **argv, HowdyDispatchFn dispatch) -> int {
		if (dispatch == nullptr) {
			std::cerr << "Error: missing Howdy dispatcher\n";
			return 1;
		}
		try {
			for (const char *name : kRuntimePathOverrideEnvVars) {
				if (unsetenv(name) != 0) {
					std::cerr << "Error: failed to clear Howdy path overrides\n";
					return 1;
				}
			}
			return dispatch(argc, argv);
		} catch (const std::exception &error) {
			std::cerr << "Error: " << error.what() << '\n';
		} catch (...) {
			std::cerr << "Error: unknown exception\n";
		}
		return 1;
	}

}  // namespace howdy::native
