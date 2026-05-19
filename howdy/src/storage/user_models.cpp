#include "storage/user_models.hpp"

#include <filesystem>
#include <fstream>
#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "common/file_security.hpp"
#include "common/user_names.hpp"
#include "config/runtime_paths.hpp"

namespace howdy::native {

namespace {

constexpr std::uintmax_t kMaxUserModelFileBytes = 1024 * 1024;
constexpr std::size_t kMaxStoredModels = 256;
constexpr std::size_t kMaxEncodingsPerModel = 32;
constexpr std::size_t kMaxEncodingLength = 1024;

}  // namespace

auto load_user_models(const std::string &user, const std::string &expected_backend)
    -> UserModelLoadResult {
  return load_user_models(user, expected_backend, default_secure_owner_uid());
}

auto load_user_models(const std::string &user, const std::string &expected_backend,
                      std::optional<uid_t> owner_uid)
    -> UserModelLoadResult {
  UserModelLoadResult result;
  const auto user_models_dir = resolve_user_models_dir();

  const auto model_path = resolve_user_model_path(user_models_dir, user);
  if (!model_path) {
    result.status = UserModelStatus::kInvalidUser;
    result.error_message = kInvalidUserNameMessage;
    return result;
  }

  if (!std::filesystem::is_regular_file(*model_path)) {
    result.status = UserModelStatus::kNoModel;
    return result;
  }

  const auto models_dir_security =
      check_secure_root_owned_directory_tree(user_models_dir,
                                             "User models directory",
                                             owner_uid);
  if (!models_dir_security.ok) {
    result.status = UserModelStatus::kInsecurePath;
    result.error_message = models_dir_security.error_message;
    return result;
  }

  const auto model_file_security = check_secure_root_owned_file_with_directory(
      *model_path, "User models directory", "User model file", owner_uid);
  if (!model_file_security.ok) {
    result.status = UserModelStatus::kInsecurePath;
    result.error_message = model_file_security.error_message;
    return result;
  }

  std::error_code size_ec;
  const auto file_size = std::filesystem::file_size(*model_path, size_ec);
  if (size_ec || file_size > kMaxUserModelFileBytes) {
    result.status = UserModelStatus::kParseError;
    result.error_message = "User model file is too large or unreadable: " +
                           model_path->string();
    return result;
  }

  std::ifstream input(*model_path);
  if (!input.is_open()) {
    result.status = UserModelStatus::kParseError;
    result.error_message = "Failed to open user model file: " + model_path->string();
    return result;
  }

  nlohmann::json models;
  try {
    input >> models;
  } catch (const nlohmann::json::exception &error) {
    result.status = UserModelStatus::kParseError;
    result.error_message = error.what();
    return result;
  }

  if (!models.is_array() || models.empty()) {
    result.status = UserModelStatus::kNoModel;
    return result;
  }

  if (models.size() > kMaxStoredModels) {
    result.status = UserModelStatus::kParseError;
    result.error_message = "Stored face model list exceeds safety limit";
    return result;
  }

  try {
    for (const auto &model : models) {
      const auto backend = model.value("backend", std::string());
      if (!backend.empty() && backend != expected_backend) {
        result.status = UserModelStatus::kIncompatibleBackend;
        result.error_message =
            "Stored face models use an incompatible backend; re-enroll required";
        return result;
      }

      const int id = model.value("id", -1);
      const std::string label = model.value("label", std::string());
      const auto data = model.find("data");
      if (data == model.end() || !data->is_array()) {
        continue;
      }

      if (data->size() > kMaxEncodingsPerModel) {
        result.status = UserModelStatus::kParseError;
        result.error_message =
            "Stored face model contains too many encodings";
        return result;
      }

      for (const auto &encoding_json : *data) {
        if (!encoding_json.is_array()) {
          continue;
        }

        if (encoding_json.empty() || encoding_json.size() > kMaxEncodingLength) {
          result.status = UserModelStatus::kParseError;
          result.error_message =
              "Stored face encoding exceeds safety limit";
          return result;
        }

        std::vector<float> encoding;
        encoding.reserve(encoding_json.size());
        for (const auto &value : encoding_json) {
          encoding.push_back(value.get<float>());
        }

        if (encoding.empty()) {
          continue;
        }

        result.stored.encodings.push_back(std::move(encoding));
        result.stored.models.push_back(EncodingModelInfo{.id = id, .label = label});
      }
    }
  } catch (const nlohmann::json::exception &error) {
    result.status = UserModelStatus::kParseError;
    result.error_message = error.what();
    return result;
  }

  result.status = result.stored.encodings.empty() ? UserModelStatus::kNoModel
                                                  : UserModelStatus::kOk;
  return result;
}

}  // namespace howdy::native
