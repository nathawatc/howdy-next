#include "compare/compare_sandbox_test_groups.hpp"

#include <cstddef>
#include <span>
#include <string_view>

auto main(int argc, char **argv) -> int {
	const auto arguments =
	    std::span(argv, argc > 0 ? static_cast<std::size_t>(argc) : std::size_t{0});
	if (arguments.size() == 3 &&
	    std::string_view(arguments.subspan(1).front()) == "--seccomp-probe") {
		return RunCompareSandboxSeccompProbe(arguments.back());
	}

	bool ok = true;
	ok &= RunCompareSandboxLimitTests();
	ok &= RunCompareSandboxSeccompTests();
	return ok ? 0 : 1;
}
