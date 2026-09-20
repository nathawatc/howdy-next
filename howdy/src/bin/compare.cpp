#include "compare/entrypoint.hpp"

#include <sys/types.h>

auto main(int argc, char **argv) -> int {
	return howdy::native::RunCompareEntrypoint(argc, argv, static_cast<uid_t>(0));
}
