#include "app/howdy_entrypoint.hpp"

#include <array>
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
			constexpr std::array path_overrides{
			    "HOWDY_CONFIG",
			    "HOWDY_MODELS_DIR",
			    "HOWDY_USER_MODELS_DIR",
			    "HOWDY_LOG_PATH",
			};
			for (const char *name : path_overrides) {
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
