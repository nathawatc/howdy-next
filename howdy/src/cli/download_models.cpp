#include "cli/download_models_cli.hpp"

#include <curl/curl.h>
#include <openssl/evp.h>

#include <array>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "config/runtime_paths.hpp"
#include "core/face_model.hpp"

namespace {

constexpr int kExitOk = 0;
constexpr int kExitAbort = 1;

struct ModelDownload {
  std::string name;
  std::string url;
  std::filesystem::path destination;
  std::optional<std::string> sha256;
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

auto trim(const std::string &value) -> std::string {
  const auto start = value.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) {
    return {};
  }
  const auto end = value.find_last_not_of(" \t\r\n");
  return value.substr(start, end - start + 1);
}

auto is_hex_sha256(const std::string &value) -> bool {
  if (value.size() != 64) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
    return std::isxdigit(ch) != 0;
  });
}

auto lower_hex(std::string value) -> std::string {
  for (char &ch : value) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return value;
}

size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
  auto *stream = static_cast<std::ofstream *>(userp);
  const auto total = size * nmemb;
  stream->write(static_cast<const char *>(contents),
                static_cast<std::streamsize>(total));
  return total;
}

size_t header_capture_callback(char *buffer, size_t size, size_t nitems,
                               void *userdata) {
  auto *etag = static_cast<std::string *>(userdata);
  const auto total = size * nitems;
  std::string line(buffer, total);
  const auto colon_pos = line.find(':');
  if (colon_pos == std::string::npos) {
    return total;
  }

  auto key = line.substr(0, colon_pos);
  for (char &ch : key) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  if (key != "etag") {
    return total;
  }

  auto value = trim(line.substr(colon_pos + 1));
  if (value.rfind("W/", 0) == 0) {
    value = value.substr(2);
  }
  if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
    value = value.substr(1, value.size() - 2);
  }
  *etag = trim(value);
  return total;
}

auto fetch_remote_sha256(const std::string &url) -> std::optional<std::string> {
  CURL *curl = curl_easy_init();
  if (curl == nullptr) {
    return std::nullopt;
  }

  std::string etag;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_capture_callback);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &etag);
  const CURLcode result = curl_easy_perform(curl);

  long status_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
  curl_easy_cleanup(curl);
  if (result != CURLE_OK || status_code < 200 || status_code >= 400) {
    return std::nullopt;
  }

  etag = trim(etag);
  if (!is_hex_sha256(etag)) {
    return std::nullopt;
  }
  return lower_hex(etag);
}

auto file_sha256(const std::filesystem::path &path) -> std::optional<std::string> {
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    return std::nullopt;
  }

  EVP_MD_CTX *context = EVP_MD_CTX_new();
  if (context == nullptr) {
    return std::nullopt;
  }
  if (EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1) {
    EVP_MD_CTX_free(context);
    return std::nullopt;
  }

  std::array<char, 8192> buffer{};
  while (input.good()) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto bytes_read = input.gcount();
    if (bytes_read > 0) {
      if (EVP_DigestUpdate(context, buffer.data(),
                           static_cast<std::size_t>(bytes_read)) != 1) {
        EVP_MD_CTX_free(context);
        return std::nullopt;
      }
    }
  }

  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int digest_length = 0;
  if (EVP_DigestFinal_ex(context, digest.data(), &digest_length) != 1) {
    EVP_MD_CTX_free(context);
    return std::nullopt;
  }
  EVP_MD_CTX_free(context);
  if (digest_length != 32U) {
    return std::nullopt;
  }

  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (unsigned int index = 0; index < digest_length; ++index) {
    out << std::setw(2) << static_cast<unsigned int>(digest[index]);
  }
  return out.str();
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

  long status_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
  curl_easy_cleanup(curl);
  output.close();
  return result == CURLE_OK && status_code >= 200 && status_code < 400;
}

}  // namespace

int download_models_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  const auto models_dir = howdy::native::resolve_models_dir();
  std::filesystem::create_directories(models_dir);

  const std::vector<ModelDownload> models = {
      {howdy::native::FaceModel::kYunetModel,
       "https://huggingface.co/opencv/face_detection_yunet/resolve/main/" +
           std::string(howdy::native::FaceModel::kYunetModel),
       models_dir / howdy::native::FaceModel::kYunetModel,
       "49f000ec501fef24739071fc7e68267d32209045b6822c0c72dce1da25726f10"},
      {howdy::native::FaceModel::kSfaceModel,
       "https://huggingface.co/opencv/face_recognition_sface/resolve/main/" +
           std::string(howdy::native::FaceModel::kSfaceModel),
       models_dir / howdy::native::FaceModel::kSfaceModel,
       "fb143eea07838aa532d1c95df5f69899974ea0140e1fba05e94204be13ed74ee"},
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

    auto expected_sha256 = model.sha256;
    if (!expected_sha256.has_value()) {
      expected_sha256 = fetch_remote_sha256(model.url);
    }
    if (!expected_sha256.has_value()) {
      std::filesystem::remove(temp_path);
      curl_global_cleanup();
      std::cout << "Failed to verify model checksum metadata: " << model.url << "\n";
      return kExitAbort;
    }

    const auto actual_sha256 = file_sha256(temp_path);
    if (!actual_sha256.has_value() ||
        lower_hex(expected_sha256.value()) != lower_hex(actual_sha256.value())) {
      std::filesystem::remove(temp_path);
      curl_global_cleanup();
      std::cout << "Checksum mismatch for " << model.name << "\n";
      std::cout << "Expected SHA256: " << expected_sha256.value() << "\n";
      if (actual_sha256.has_value()) {
        std::cout << "Actual SHA256:   " << actual_sha256.value() << "\n";
      }
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
