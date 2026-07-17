#include "auth_helper/command.hpp"

auto main(int argc, char **argv) -> int {
	return howdy::native::auth_helper::command::run(argc, argv);
}
