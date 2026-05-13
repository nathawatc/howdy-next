#include "download_models_cli.hpp"

#include <curl/curl.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "../config/runtime_paths.hpp"
#include "../core/face_model.hpp"

namespace {

constexpr int kExitOk = 0;
constexpr int kExitAbort = 1;

struct ModelDownload {
  std::string name;
  std::string url;
  std::filesystem::path destination;
};

auto bad_model_download(const std::filesystem::path &path) -> bool {
  if (!std::filesystem::is_regular_file(path)) {
    return true;
  }

  std::ifstream input(path, std::ios::binary);
  std::string header(256, '\0');
  input.read(header.data(), static_cast<std::streamsize>(header.size()));
  header.resize(static_cast<std::size_t>(input.gcount()));
  return header.rfind("version https://git-lfs.github.com/spec/v1", 0) == 0 ||
         (!header.empty() && header.front() == '<');
}

size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
  auto *stream = static_cast<std::ofstream *>(userp);
  const auto total = size * nmemb;
  stream->write(static_cast<const char *>(contents),
                static_cast<std::streamsize>(total));
  return total;
}

auto download_file(const std::string &url, const std::filesystem::path &temp_path)
    -> bool {
  CURL *curl = curl_easy_init();
  if (curl == nullptr) {
    return false;
  }

  std::ofstream output(temp_path, std::ios::binary);
  if (!output.is_open()) {
    curl_easy_cleanup(curl);
    return false;
  }

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &output);
  const CURLcode result = curl_easy_perform(curl);
  curl_easy_cleanup(curl);
  output.close();
  return result == CURLE_OK;
}

}  // namespace

int download_models_main(int, char **) {
  const auto models_dir = howdy::native::resolve_models_dir();
  std::filesystem::create_directories(models_dir);

  const std::vector<ModelDownload> models = {
      {howdy::native::FaceModel::kYunetModel,
       "https://huggingface.co/opencv/face_detection_yunet/resolve/main/" +
           std::string(howdy::native::FaceModel::kYunetModel),
       models_dir / howdy::native::FaceModel::kYunetModel},
      {howdy::native::FaceModel::kSfaceModel,
       "https://huggingface.co/opencv/face_recognition_sface/resolve/main/" +
           std::string(howdy::native::FaceModel::kSfaceModel),
       models_dir / howdy::native::FaceModel::kSfaceModel},
  };

  curl_global_init(CURL_GLOBAL_DEFAULT);
  for (const auto &model : models) {
    if (std::filesystem::exists(model.destination) &&
        !bad_model_download(model.destination)) {
      std::cout << "Model already exists: " << model.destination.string() << "\n";
      continue;
    }
    if (std::filesystem::exists(model.destination)) {
      std::cout << "Replacing invalid model download: "
                << model.destination.string() << "\n";
    }

    std::cout << "Downloading " << model.name << "\n";
    const auto temp_path = model.destination.string() + ".tmp";
    if (!download_file(model.url, temp_path)) {
      std::filesystem::remove(temp_path);
      curl_global_cleanup();
      std::cout << "Failed to download model: " << model.url << "\n";
      return kExitAbort;
    }

    if (bad_model_download(temp_path)) {
      std::filesystem::remove(temp_path);
      curl_global_cleanup();
      std::cout << "Downloaded file is not an ONNX model: " << model.url << "\n";
      return kExitAbort;
    }

    std::error_code ec;
    std::filesystem::remove(model.destination, ec);
    std::filesystem::rename(temp_path, model.destination);
  }
  curl_global_cleanup();

  std::cout << "OpenCV face models ready in: " << models_dir.string() << "\n";
  return kExitOk;
}
