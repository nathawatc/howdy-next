#pragma once

#include <string>
#include <vector>

namespace howdy::native {

enum class UserModelStatus {
  kOk,
  kNoModel,
  kIncompatibleBackend,
  kParseError,
};

struct EncodingModelInfo {
  int id = -1;
  std::string label;
};

struct StoredEncodings {
  std::vector<std::vector<float>> encodings;
  std::vector<EncodingModelInfo> models;
};

struct UserModelLoadResult {
  UserModelStatus status = UserModelStatus::kNoModel;
  std::string error_message;
  StoredEncodings stored;
};

auto load_user_models(const std::string &user, const std::string &expected_backend)
    -> UserModelLoadResult;

}  // namespace howdy::native
