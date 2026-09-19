#include "app/howdy_entrypoint.hpp"

#include <exception>
#include <iostream>

namespace howdy::native {

	auto RunHowdyEntrypoint(int argc, char **argv, HowdyDispatchFn dispatch) -> int {
		if (dispatch == nullptr) {
			std::cerr << "Error: missing Howdy dispatcher\n";
			return 1;
		}
		try {
			return dispatch(argc, argv);
		} catch (const std::exception &error) {
			std::cerr << "Error: " << error.what() << '\n';
		} catch (...) {
			std::cerr << "Error: unknown exception\n";
		}
		return 1;
	}

}  // namespace howdy::native
