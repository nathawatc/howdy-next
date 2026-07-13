#include "cli/howdy_cli.hpp"

#include <exception>
#include <iostream>

#ifndef HOWDY_MAIN_ENTRYPOINT
#	define HOWDY_MAIN_ENTRYPOINT main
#endif

#ifndef HOWDY_MAIN_DISPATCH
#	define HOWDY_MAIN_DISPATCH howdy_main
#endif

int HOWDY_MAIN_DISPATCH(int argc, char **argv);

int HOWDY_MAIN_ENTRYPOINT(int argc, char **argv) {
	try {
		return HOWDY_MAIN_DISPATCH(argc, argv);
	} catch (const std::exception &error) {
		std::cerr << "Error: " << error.what() << '\n';
	} catch (...) {
		std::cerr << "Error: unknown exception\n";
	}

	return 1;
}
