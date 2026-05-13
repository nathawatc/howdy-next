#pragma once

#include <string>

#include "common/compare_exit.hpp"

namespace howdy::native {

enum class BrightnessDecision {
  kBlackFrame,
  kTooDark,
  kProcessFrame,
};

auto update_best_score(float current, float score, const std::string &metric)
    -> float;

auto classify_brightness(double hist_total, float darkness, float dark_threshold)
    -> BrightnessDecision;

auto timeout_exit(int dark_tries, int valid_frames) -> CompareExit;

}  // namespace howdy::native
