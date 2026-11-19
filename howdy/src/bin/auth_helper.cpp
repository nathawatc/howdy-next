#include "auth_helper/command.hpp"

#include <cstdio>
#include <iostream>

#include <unistd.h>

auto main(int argc, char **argv) -> int {
	const int exit_code = howdy::native::auth_helper::command::Run(argc, argv);
	// A successful prepare ends with the completion record PAM may rely on. Flush and skip static
	// teardown, which could still end the helper abnormally after that record.
	std::cout.flush();
	(void)std::fflush(nullptr);
	_exit(exit_code);
}
