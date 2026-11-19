#include "compare/entrypoint.hpp"

#include <unistd.h>

#include <sys/types.h>

auto main(int argc, char **argv) -> int {
	// RunCompareEntrypoint flushes the output and then reports the verdict PAM may rely on as its
	// last step. Skip static teardown, which could still end compare abnormally after that.
	_exit(howdy::native::RunCompareEntrypoint(argc, argv, static_cast<uid_t>(0)));
}
