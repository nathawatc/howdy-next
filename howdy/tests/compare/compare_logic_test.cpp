#include "compare/logic.hpp"
#include "protocol/compare_exit.hpp"
#include "test_support.hpp"

#include <sstream>
#include <stdexcept>
#include <string>

#include <opencv2/core/exception.hpp>

namespace {

	using howdy::test::Expect;

}  // namespace

auto main() -> int {
	bool ok = true;

	ok &= Expect(howdy::native::TimeoutExit(2, 2) == howdy::native::CompareExit::kTooDark,
	             "all valid frames too dark returns too-dark exit");
	ok &= Expect(howdy::native::TimeoutExit(0, 0) == howdy::native::CompareExit::kTimeoutReached,
	             "no dark frames returns timeout exit");
	ok &= Expect(howdy::native::TimeoutExit(1, 2) == howdy::native::CompareExit::kTimeoutReached,
	             "mixed valid frames returns timeout exit");

	ok &= Expect(static_cast<int>(howdy::native::CompareExit::kSuccess) == 0,
	             "success exit code remains stable");
	ok &= Expect(static_cast<int>(howdy::native::CompareExit::kNoFaceModel) == 10,
	             "no-face-model exit code remains stable for PAM");
	ok &= Expect(static_cast<int>(howdy::native::CompareExit::kTimeoutReached) == 11,
	             "timeout exit code remains stable for PAM");
	ok &= Expect(static_cast<int>(howdy::native::CompareExit::kAbort) == 12,
	             "abort exit code remains stable for PAM");
	ok &= Expect(static_cast<int>(howdy::native::CompareExit::kTooDark) == 13,
	             "too-dark exit code remains stable for PAM");
	ok &= Expect(static_cast<int>(howdy::native::CompareExit::kInvalidDevice) == 14,
	             "invalid-device exit code remains stable for PAM");

	{
		std::ostringstream  stream;
		const cv::Exception error(cv::Error::StsError, "simulated OpenCV failure", "detect",
		                          "compare.cpp", 42);
		ok &= Expect(howdy::native::CompareAbortFromCvException(
		                 error, stream, "test compare path") == howdy::native::CompareExit::kAbort,
		             "OpenCV exception maps to abort");
		ok &= Expect(stream.str().contains("OpenCV exception during test compare path"),
		             "OpenCV exception context is logged");
	}

	{
		std::ostringstream       stream;
		const std::runtime_error error("simulated std failure");
		ok &= Expect(howdy::native::CompareAbortFromException(error, stream, "test compare path") ==
		                 howdy::native::CompareExit::kAbort,
		             "std exception maps to abort");
		ok &= Expect(stream.str().contains("Unhandled exception during test compare path"),
		             "std exception context is logged");
	}

	{
		std::ostringstream stream;
		ok &= Expect(howdy::native::CompareAbortFromUnknownException(stream, "test compare path") ==
		                 howdy::native::CompareExit::kAbort,
		             "unknown exception maps to abort");
		ok &= Expect(stream.str().contains("Unknown exception during test compare path"),
		             "unknown exception context is logged");
	}

	return ok ? 0 : 1;
}
