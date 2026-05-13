#pragma once

namespace howdy::native {

enum class CompareExit {
  kSuccess = 0,
  kAbort = 1,
  kNoFaceModel = 10,
  kTimeoutReached = 11,
  kTooDark = 13,
  kInvalidDevice = 14,
  kRubberstamp = 15,
};

}  // namespace howdy::native
