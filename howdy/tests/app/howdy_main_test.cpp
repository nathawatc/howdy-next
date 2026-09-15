#include "test_support.hpp"

#include <cstdint>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

	using howdy::test::Expect;

	enum class DispatchBehavior : std::uint8_t {
		kReturn,
		kStdException,
		kUnknownException,
	};

	DispatchBehavior dispatch_behavior = DispatchBehavior::kReturn;
	int              dispatch_result   = 0;

}  // namespace

auto HowdyMainTestEntry(int argc, char **argv) -> int;
auto HowdyMainTestDispatch(int argc, char **argv) -> int;

auto HowdyMainTestDispatch(int argc, char **argv) -> int {
	(void)argc;
	(void)argv;

	switch (dispatch_behavior) {
		case DispatchBehavior::kReturn:
			return dispatch_result;
		case DispatchBehavior::kStdException:
			throw std::runtime_error("dispatch exception");
		case DispatchBehavior::kUnknownException:
			throw 1;
	}

	return 1;
}

namespace {

	struct RunResult {
		int         status;
		std::string error;
	};

	auto Run() -> RunResult {
		std::ostringstream error;
		auto              *old_error = std::cerr.rdbuf(error.rdbuf());
		const int          status    = HowdyMainTestEntry(0, nullptr);
		std::cerr.rdbuf(old_error);
		return {
		    .status = status,
		    .error  = error.str(),
		};
	}

}  // namespace

auto main() -> int {
	bool ok = true;

	dispatch_behavior               = DispatchBehavior::kStdException;
	const auto std_exception_result = Run();
	ok &= Expect(std_exception_result.status == 1 &&
	                 std_exception_result.error == "Error: dispatch exception\n",
	             "std::exception returns stable diagnostic and exit code");

	dispatch_behavior         = DispatchBehavior::kUnknownException;
	const auto unknown_result = Run();
	ok &= Expect(unknown_result.status == 1 && unknown_result.error == "Error: unknown exception\n",
	             "unknown exception returns stable diagnostic and exit code");

	dispatch_behavior        = DispatchBehavior::kReturn;
	dispatch_result          = 23;
	const auto normal_result = Run();
	ok &= Expect(normal_result.status == 23 && normal_result.error.empty(),
	             "normal dispatcher return code is preserved");

	return ok ? 0 : 1;
}
