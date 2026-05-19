#pragma once

#include <sys/types.h>

#include <optional>
#include <string>
#include <vector>

namespace howdy::native {

enum class UserModelStatus {
  kOk,
  kNoModel,
  kIncompatibleBackend,
  kParseError,
  kInsecurePath,
  kInvalidUser,
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
auto load_user_models(const std::string &user, const std::string &expected_backend,
                      std::optional<uid_t> owner_uid)
    -> UserModelLoadResult;

}  // namespace howdy::native
