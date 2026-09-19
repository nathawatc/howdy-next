#include "app/howdy.hpp"
#include "app/howdy_entrypoint.hpp"

auto main(int argc, char **argv) -> int {
	return howdy::native::RunHowdyEntrypoint(argc, argv, HowdyMain);
}
