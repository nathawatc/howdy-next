#include "common/compare_logic.hpp"

#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

auto expect(bool condition, const std::string &message) -> bool {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    return false;
  }
  return true;
}

}  // namespace

auto main() -> int {
  bool ok = true;

  ok &= expect(howdy::native::update_best_score(
                   std::numeric_limits<float>::quiet_NaN(), 0.5F, "cosine") == 0.5F,
               "nan best score initializes to first score");
  ok &= expect(howdy::native::update_best_score(0.5F, 0.8F, "cosine") == 0.8F,
               "cosine keeps max");
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
  ok &= expect(howdy::native::classify_brightness(5.0, 70.0F, 50.0F) ==
                   howdy::native::BrightnessDecision::kTooDark,
               "above threshold is too dark");
  ok &= expect(howdy::native::classify_brightness(5.0, 40.0F, 50.0F) ==
                   howdy::native::BrightnessDecision::kProcessFrame,
               "below threshold is processable");

  ok &= expect(howdy::native::timeout_exit(2, 2) ==
                   howdy::native::CompareExit::kTooDark,
               "all valid frames too dark returns too-dark exit");
  ok &= expect(howdy::native::timeout_exit(0, 0) ==
                   howdy::native::CompareExit::kTimeoutReached,
               "no dark frames returns timeout exit");
  ok &= expect(howdy::native::timeout_exit(1, 2) ==
                   howdy::native::CompareExit::kTimeoutReached,
               "mixed valid frames returns timeout exit");

  {
    std::ostringstream stream;
    const cv::Exception error(cv::Error::StsError, "simulated OpenCV failure",
                              "detect", "compare.cpp", 42);
    ok &= expect(howdy::native::compare_abort_from_cv_exception(
                     error, stream, "test compare path") ==
                     howdy::native::CompareExit::kAbort,
                 "OpenCV exception maps to abort");
    ok &= expect(stream.str().find("OpenCV exception during test compare path") !=
                     std::string::npos,
                 "OpenCV exception context is logged");
  }

  {
    std::ostringstream stream;
    const std::runtime_error error("simulated std failure");
    ok &= expect(howdy::native::compare_abort_from_exception(
                     error, stream, "test compare path") ==
                     howdy::native::CompareExit::kAbort,
                 "std exception maps to abort");
    ok &= expect(stream.str().find("Unhandled exception during test compare path") !=
                     std::string::npos,
                 "std exception context is logged");
  }

  {
    std::ostringstream stream;
    ok &= expect(howdy::native::compare_abort_from_unknown_exception(
                     stream, "test compare path") ==
                     howdy::native::CompareExit::kAbort,
                 "unknown exception maps to abort");
    ok &= expect(stream.str().find("Unknown exception during test compare path") !=
                     std::string::npos,
                 "unknown exception context is logged");
  }

  if (!ok) {
    return 1;
  }
  return 0;
}
