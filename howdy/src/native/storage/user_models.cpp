#include "howdy/storage/user_models.hpp"

#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "howdy/config/runtime_paths.hpp"

namespace howdy::native {

auto load_user_models(const std::string &user, const std::string &expected_backend)
    -> UserModelLoadResult {
  UserModelLoadResult result;

  const auto model_path = resolve_user_models_dir() / (user + ".dat");
  if (!std::filesystem::is_regular_file(model_path)) {
    result.status = UserModelStatus::kNoModel;
    return result;
  }

  std::ifstream input(model_path);
  if (!input.is_open()) {
    result.status = UserModelStatus::kParseError;
    result.error_message = "Failed to open user model file: " + model_path.string();
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

    for (const auto &encoding_json : *data) {
      if (!encoding_json.is_array()) {
        continue;
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

  result.status = result.stored.encodings.empty() ? UserModelStatus::kNoModel
                                                  : UserModelStatus::kOk;
  return result;
}

}  // namespace howdy::native
