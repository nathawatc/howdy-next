#include "storage/user_models.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

auto write_file(const std::filesystem::path &path, const std::string &content)
    -> bool {
  std::ofstream out(path);
  if (!out.is_open()) {
    return false;
  }
  out << content;
  return out.good();
}

auto expect(bool condition, const std::string &message) -> bool {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    return false;
  }
  return true;
}

}  // namespace

auto main() -> int {
  namespace fs = std::filesystem;

  bool ok = true;
  const auto temp_root = fs::temp_directory_path() / "howdy-user-models-test";
  std::error_code ec;
  fs::remove_all(temp_root, ec);
  fs::create_directories(temp_root, ec);
  ok &= expect(!ec, "create temp root");

  const auto models_dir = temp_root / "models";
  fs::create_directories(models_dir, ec);
  ok &= expect(!ec, "create models dir");
  setenv("HOWDY_USER_MODELS_DIR", models_dir.c_str(), 1);

  const std::string backend = "opencv_dnn_sface";
  {
    const auto result = howdy::native::load_user_models("alice", backend);
    ok &= expect(result.status == howdy::native::UserModelStatus::kNoModel,
                 "missing file returns kNoModel");
  }

  const auto model_path = models_dir / "alice.dat";
  ok &= expect(write_file(model_path, "not-json"), "write malformed model file");
  {
    const auto result = howdy::native::load_user_models("alice", backend);
    ok &= expect(result.status == howdy::native::UserModelStatus::kParseError,
                 "malformed json returns kParseError");
  }

  ok &= expect(write_file(model_path, "[]"), "write empty model list");
  {
    const auto result = howdy::native::load_user_models("alice", backend);
    ok &= expect(result.status == howdy::native::UserModelStatus::kNoModel,
                 "empty model list returns kNoModel");
  }

  ok &= expect(write_file(model_path,
                          R"([{"id":1,"label":"bad","backend":"other_backend","data":[[0.1,0.2]]}])"),
               "write incompatible backend model");
  {
    const auto result = howdy::native::load_user_models("alice", backend);
    ok &= expect(result.status ==
                     howdy::native::UserModelStatus::kIncompatibleBackend,
                 "incompatible backend is detected");
  }

  ok &= expect(write_file(model_path,
                          R"([
{"id":7,"label":"first","backend":"opencv_dnn_sface","data":[[0.1,0.2],[0.3,0.4]]},
{"id":8,"label":"second","backend":"opencv_dnn_sface","data":["ignored",[1.1,1.2,1.3]]}
])"),
               "write valid models");
  {
    const auto result = howdy::native::load_user_models("alice", backend);
    ok &= expect(result.status == howdy::native::UserModelStatus::kOk,
                 "valid models return kOk");
    ok &= expect(result.stored.encodings.size() == 3,
                 "three valid encodings loaded");
    ok &= expect(result.stored.models.size() == 3,
                 "model metadata count matches encodings");
    ok &= expect(result.stored.models[0].id == 7 &&
                     result.stored.models[0].label == "first",
                 "first model metadata preserved");
    ok &= expect(result.stored.models[2].id == 8 &&
                     result.stored.models[2].label == "second",
                 "second model metadata preserved");
  }

  fs::remove_all(temp_root, ec);
  unsetenv("HOWDY_USER_MODELS_DIR");

  if (!ok) {
    return 1;
  }
  return 0;
}
