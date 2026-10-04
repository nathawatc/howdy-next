#include "app/howdy_entrypoint.hpp"
#include "config/runtime_paths.hpp"
#include "paths.hpp"
#include "test_support.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

#include <stdlib.h>

namespace {

	using howdy::test::Expect;

	enum class DispatchBehavior : std::uint8_t {
		kReturn,
		kStdException,
		kUnknownException,
	};

	struct DispatchState {
		DispatchBehavior behavior = DispatchBehavior::kReturn;
		int              result   = 0;
	};

	auto TestDispatchState() -> DispatchState & {
		static DispatchState state;
		return state;
	}

}  // namespace

namespace {

	auto TestDispatch(int argc, char **argv) -> int {
		(void)argc;
		(void)argv;

		switch (TestDispatchState().behavior) {
			case DispatchBehavior::kReturn:
				return TestDispatchState().result;
			case DispatchBehavior::kStdException:
				throw std::runtime_error("dispatch exception");
			case DispatchBehavior::kUnknownException:
				howdy::test::ThrowNonStandardExceptionForTest();
		}

		return 1;
	}

	auto CheckProductionPaths([[maybe_unused]] int argc, [[maybe_unused]] char **argv) -> int {
		return howdy::native::ResolveConfigPath() == kConfiguredConfigPath &&
		               howdy::native::ResolveModelsDir() == kConfiguredModelsDir &&
		               howdy::native::ResolveUserModelsDir() == kConfiguredUserModelsDir &&
		               howdy::native::ResolveLogPath() == kConfiguredLogPath &&
		               std::getenv("HOWDY_CONFIG") == nullptr &&
		               std::getenv("HOWDY_MODELS_DIR") == nullptr &&
		               std::getenv("HOWDY_USER_MODELS_DIR") == nullptr &&
		               std::getenv("HOWDY_LOG_PATH") == nullptr
		           ? 71
				   : 1;
	}

}  // namespace

namespace {

	struct RunResult {
		int         status;
		std::string error;
	};

	auto Run() -> RunResult {
		std::ostringstream const error;
		auto                    *old_error = std::cerr.rdbuf(error.rdbuf());
		const int status = howdy::native::RunHowdyEntrypoint(0, nullptr, TestDispatch);
		std::cerr.rdbuf(old_error);
		return {
		    .status = status,
		    .error  = error.str(),
		};
	}

}  // namespace

auto main() -> int {
	bool ok = true;

	TestDispatchState().behavior    = DispatchBehavior::kStdException;
	const auto std_exception_result = Run();
	ok &= Expect(std_exception_result.status == 1 &&
	                 std_exception_result.error == "Error: dispatch exception\n",
	             "std::exception returns stable diagnostic and exit code");

	TestDispatchState().behavior = DispatchBehavior::kUnknownException;
	const auto unknown_result    = Run();
	ok &= Expect(unknown_result.status == 1 && unknown_result.error == "Error: unknown exception\n",
	             "unknown exception returns stable diagnostic and exit code");

	TestDispatchState().behavior = DispatchBehavior::kReturn;
	TestDispatchState().result   = 23;
	const auto normal_result     = Run();
	ok &= Expect(normal_result.status == 23 && normal_result.error.empty(),
	             "normal dispatcher return code is preserved");

	constexpr std::array path_overrides{
	    "HOWDY_CONFIG",
	    "HOWDY_MODELS_DIR",
	    "HOWDY_USER_MODELS_DIR",
	    "HOWDY_LOG_PATH",
	};
	for (const char *name : path_overrides) {
		ok &= Expect(setenv(name, "/tmp/howdy-redirected-path", 1) == 0,
		             "set path override for production boundary test");
	}
	ok &= Expect(howdy::native::RunHowdyEntrypoint(0, nullptr, CheckProductionPaths) == 71,
	             "production dispatch clears all four path overrides");

	ok &= Expect(setenv("HOWDY_USER_MODELS_DIR", "/tmp/howdy-staged-models", 1) == 0,
	             "set staged models override after production dispatch");
	ok &= Expect(howdy::native::ResolveUserModelsDir() == "/tmp/howdy-staged-models",
	             "internal resolver retains staged override support");

	return ok ? 0 : 1;
}
