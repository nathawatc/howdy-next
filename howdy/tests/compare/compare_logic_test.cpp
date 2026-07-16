#include "compare/logic.hpp"
#include "test_support.hpp"

#include <cmath>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

	using howdy::test::expect;

	auto expect_near(double actual, double expected, double tolerance, const std::string &message)
	    -> bool {
		return expect(std::fabs(actual - expected) <= tolerance, message);
	}

}  // namespace

auto main() -> int {
	bool ok = true;

	ok &= expect(howdy::native::update_best_score(std::numeric_limits<float>::quiet_NaN(), 0.5F,
	                                              "cosine") == 0.5F,
	             "nan best score initializes to first score");
	ok &=
	    expect(howdy::native::update_best_score(0.5F, 0.8F, "cosine") == 0.8F, "cosine keeps max");
	ok &= expect(howdy::native::update_best_score(0.8F, 0.5F, "cosine") == 0.8F,
	             "cosine ignores lower score");
	ok &= expect(howdy::native::update_best_score(2.0F, 1.5F, "l2") == 1.5F,
	             "distance metric keeps min");
	ok &= expect(howdy::native::update_best_score(1.5F, 2.0F, "l2") == 1.5F,
	             "distance metric ignores higher score");

	ok &= expect(howdy::native::classify_brightness(0.0, 10.0F, 50.0F) ==
	                 howdy::native::BrightnessDecision::kBlackFrame,
	             "zero histogram is black frame");
	ok &= expect(howdy::native::classify_brightness(5.0, 100.0F, 50.0F) ==
	                 howdy::native::BrightnessDecision::kBlackFrame,
	             "100 percent darkness is black frame");
	ok &= expect(howdy::native::classify_brightness(5.0, 100.0F, 100.0F) ==
	                 howdy::native::BrightnessDecision::kBlackFrame,
	             "black frame classification wins at maximum threshold");
	ok &= expect(howdy::native::classify_brightness(5.0, 70.0F, 50.0F) ==
	                 howdy::native::BrightnessDecision::kTooDark,
	             "above threshold is too dark");
	ok &= expect(howdy::native::classify_brightness(5.0, 40.0F, 50.0F) ==
	                 howdy::native::BrightnessDecision::kProcessFrame,
	             "below threshold is processable");

	ok &= expect(howdy::native::timeout_exit(2, 2) == howdy::native::CompareExit::kTooDark,
	             "all valid frames too dark returns too-dark exit");
	ok &= expect(howdy::native::timeout_exit(0, 0) == howdy::native::CompareExit::kTimeoutReached,
	             "no dark frames returns timeout exit");
	ok &= expect(howdy::native::timeout_exit(1, 2) == howdy::native::CompareExit::kTimeoutReached,
	             "mixed valid frames returns timeout exit");

	ok &= expect_near(
	    howdy::native::compare_resize_scale({.width = 640, .height = 480, .rotation = 0}, 320.0F),
	    320.0 / 480.0, 0.000001, "landscape resize caps by frame height");
	ok &= expect_near(
	    howdy::native::compare_resize_scale({.width = 640, .height = 480, .rotation = 2}, 320.0F),
	    320.0 / 640.0, 0.000001, "portrait resize caps by rotated frame height");
	ok &= expect(howdy::native::compare_resize_scale({.width = 340, .height = 340, .rotation = 0},
	                                                 1024.0F) == 1.0,
	             "resize cap does not upscale small Brio frames");
	ok &= expect(howdy::native::compare_resize_scale({.width = 640, .height = 0, .rotation = 0},
	                                                 320.0F) == 1.0,
	             "invalid capture height does not force huge upscale");
	ok &= expect(howdy::native::compare_resize_scale({.width = 640, .height = 480, .rotation = 0},
	                                                 -1.0F) == 1.0,
	             "negative resize cap is ignored");
	ok &= expect(howdy::native::compare_resize_scale({.width = 640, .height = 480, .rotation = 0},
	                                                 std::numeric_limits<float>::infinity()) == 1.0,
	             "non-finite resize cap is ignored");

	ok &= expect(static_cast<int>(howdy::native::CompareExit::kSuccess) == 0,
	             "success exit code remains stable");
	ok &= expect(static_cast<int>(howdy::native::CompareExit::kNoFaceModel) == 10,
	             "no-face-model exit code remains stable for PAM");
	ok &= expect(static_cast<int>(howdy::native::CompareExit::kTimeoutReached) == 11,
	             "timeout exit code remains stable for PAM");
	ok &= expect(static_cast<int>(howdy::native::CompareExit::kAbort) == 12,
	             "abort exit code remains stable for PAM");
	ok &= expect(static_cast<int>(howdy::native::CompareExit::kTooDark) == 13,
	             "too-dark exit code remains stable for PAM");
	ok &= expect(static_cast<int>(howdy::native::CompareExit::kInvalidDevice) == 14,
	             "invalid-device exit code remains stable for PAM");

	{
		std::ostringstream  stream;
		const cv::Exception error(cv::Error::StsError, "simulated OpenCV failure", "detect",
		                          "compare.cpp", 42);
		ok &= expect(howdy::native::compare_abort_from_cv_exception(
		                 error, stream, "test compare path") == howdy::native::CompareExit::kAbort,
		             "OpenCV exception maps to abort");
		ok &= expect(stream.str().contains("OpenCV exception during test compare path"),
		             "OpenCV exception context is logged");
	}

	{
		std::ostringstream       stream;
		const std::runtime_error error("simulated std failure");
		ok &= expect(howdy::native::compare_abort_from_exception(
		                 error, stream, "test compare path") == howdy::native::CompareExit::kAbort,
		             "std exception maps to abort");
		ok &= expect(stream.str().contains("Unhandled exception during test compare path"),
		             "std exception context is logged");
	}

	{
		std::ostringstream stream;
		ok &= expect(howdy::native::compare_abort_from_unknown_exception(
		                 stream, "test compare path") == howdy::native::CompareExit::kAbort,
		             "unknown exception maps to abort");
		ok &= expect(stream.str().contains("Unknown exception during test compare path"),
		             "unknown exception context is logged");
	}

	if (!ok) {
		return 1;
	}
	return 0;
}
