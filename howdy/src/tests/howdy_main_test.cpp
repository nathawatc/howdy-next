#include <cstdint>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

	enum class DispatchBehavior : std::uint8_t {
		kReturn,
		kStdException,
		kUnknownException,
	};

	DispatchBehavior dispatch_behavior = DispatchBehavior::kReturn;
	int              dispatch_result   = 0;

}  // namespace

auto howdy_main_test_entry(int argc, char **argv) -> int;
auto howdy_main_test_dispatch(int argc, char **argv) -> int;

auto howdy_main_test_dispatch(int argc, char **argv) -> int {
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

	auto run() -> RunResult {
		std::ostringstream error;
		auto              *old_error = std::cerr.rdbuf(error.rdbuf());
		const int          status    = howdy_main_test_entry(0, nullptr);
		std::cerr.rdbuf(old_error);
		return {
		    .status = status,
		    .error  = error.str(),
		};
	}

	auto expect(bool condition, std::string_view message) -> bool {
		if (!condition) {
			std::cerr << "FAIL: " << message << '\n';
		}
		return condition;
	}

}  // namespace

auto main() -> int {
	bool ok = true;

	dispatch_behavior               = DispatchBehavior::kStdException;
	const auto std_exception_result = run();
	ok &= expect(std_exception_result.status == 1 &&
	                 std_exception_result.error == "Error: dispatch exception\n",
	             "std::exception returns stable diagnostic and exit code");

	dispatch_behavior         = DispatchBehavior::kUnknownException;
	const auto unknown_result = run();
	ok &= expect(unknown_result.status == 1 && unknown_result.error == "Error: unknown exception\n",
	             "unknown exception returns stable diagnostic and exit code");

	dispatch_behavior        = DispatchBehavior::kReturn;
	dispatch_result          = 23;
	const auto normal_result = run();
	ok &= expect(normal_result.status == 23 && normal_result.error.empty(),
	             "normal dispatcher return code is preserved");

	return ok ? 0 : 1;
}
