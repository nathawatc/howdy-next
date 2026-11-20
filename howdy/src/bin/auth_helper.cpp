#include "auth_helper/command.hpp"

#include <cstdio>
#include <iostream>

#include <unistd.h>

auto main(int argc, char **argv) -> int {
	const int exit_code = howdy::native::auth_helper::command::Run(argc, argv);
	// The result is already sent; flush and skip static teardown, which has nothing left to do.
	std::cout.flush();
	(void)std::fflush(nullptr);
	_exit(exit_code);
}
