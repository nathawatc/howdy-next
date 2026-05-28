#include "common/compare_logic.hpp"

#include <algorithm>
#include <cmath>
#include <ostream>

namespace howdy::native {

auto update_best_score(float current, float score, const std::string &metric)
    -> float {
  if (std::isnan(current)) {
    return score;
  }
  if (metric == "cosine") {
    return std::max(current, score);
  }
  return std::min(current, score);
}

auto classify_brightness(double hist_total, float darkness, float dark_threshold)
    -> BrightnessDecision {
  if (hist_total == 0.0 || darkness == 100.0F) {
    return BrightnessDecision::kBlackFrame;
  }
  if (darkness > dark_threshold) {
    return BrightnessDecision::kTooDark;
  }
  return BrightnessDecision::kProcessFrame;
}

auto timeout_exit(int dark_tries, int valid_frames) -> CompareExit {
  if (dark_tries > 0 && valid_frames == dark_tries) {
    return CompareExit::kTooDark;
  }
  return CompareExit::kTimeoutReached;
}

auto compare_abort_from_cv_exception(const cv::Exception &error,
                                     std::ostream &stream,
                                     std::string_view context) -> CompareExit {
  stream << "OpenCV exception during " << context << ": " << error.what()
         << "\n";
  return CompareExit::kAbort;
}

auto compare_abort_from_exception(const std::exception &error,
                                  std::ostream &stream,
                                  std::string_view context) -> CompareExit {
  stream << "Unhandled exception during " << context << ": " << error.what()
         << "\n";
  return CompareExit::kAbort;
}

auto compare_abort_from_unknown_exception(std::ostream &stream,
                                          std::string_view context)
    -> CompareExit {
  stream << "Unknown exception during " << context << "\n";
  return CompareExit::kAbort;
}

}  // namespace howdy::native
