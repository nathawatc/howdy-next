#include "cli/download_models_cli.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

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

#include "common/atomic_files.hpp"
#include "common/file_security.hpp"
#include "config/runtime_paths.hpp"
#include "core/face_model.hpp"

namespace {

constexpr int kExitOk = 0;
constexpr int kExitAbort = 1;
constexpr long kConnectTimeoutSeconds = 15;
constexpr long kTransferTimeoutSeconds = 300;
constexpr long kLowSpeedBytesPerSecond = 1024;
constexpr long kLowSpeedTimeoutSeconds = 30;
constexpr curl_off_t kMaxDownloadBytes = 100 * 1024 * 1024;

struct ModelDownload {
  std::string name;
  std::string url;
  std::filesystem::path destination;
  std::optional<std::string> sha256;
};

struct StagedDownloadFile {
  int fd = -1;
  std::filesystem::path path;
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

void configure_transfer_policy(CURL *curl) {
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, kConnectTimeoutSeconds);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, kTransferTimeoutSeconds);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, kLowSpeedBytesPerSecond);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, kLowSpeedTimeoutSeconds);
  curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, kMaxDownloadBytes);
#ifdef CURLOPT_PROTOCOLS_STR
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
#endif
#ifdef CURLOPT_REDIR_PROTOCOLS_STR
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#endif
}

size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
  auto *fd = static_cast<int *>(userp);
  const auto total = size * nmemb;

  const char *cursor = static_cast<const char *>(contents);
  std::size_t remaining = total;
  while (remaining > 0) {
    const auto written = write(*fd, cursor, remaining);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return 0;
    }
    cursor += written;
    remaining -= static_cast<std::size_t>(written);
  }
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
  configure_transfer_policy(curl);
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

auto cleanup_staged_download(StagedDownloadFile &staged) -> void {
  if (staged.fd >= 0) {
    close(staged.fd);
    staged.fd = -1;
  }
  std::error_code ec;
  std::filesystem::remove(staged.path, ec);
}

auto prepare_staged_download(const std::filesystem::path &destination)
    -> std::optional<StagedDownloadFile> {
  const auto parent = destination.parent_path();
  std::filesystem::create_directories(parent);

  if (std::filesystem::exists(parent)) {
    const auto dir_security =
        howdy::native::check_secure_root_owned_directory(parent, "Models directory");
    if (!dir_security.ok) {
      return std::nullopt;
    }
  }

  struct stat current_stat {};
  const bool have_current_stat = lstat(destination.c_str(), &current_stat) == 0;
  if (have_current_stat && !S_ISREG(current_stat.st_mode)) {
    return std::nullopt;
  }

  std::string temp_template = (parent / ".howdy-download-XXXXXX").string();
  std::vector<char> writable(temp_template.begin(), temp_template.end());
  writable.push_back('\0');

  const int fd = mkstemp(writable.data());
  if (fd < 0) {
    return std::nullopt;
  }

  if (have_current_stat) {
    if (fchmod(fd, current_stat.st_mode & 07777) != 0 ||
        fchown(fd, current_stat.st_uid, current_stat.st_gid) != 0) {
      close(fd);
      std::error_code ec;
      std::filesystem::remove(writable.data(), ec);
      return std::nullopt;
    }
  } else if (fchmod(fd, howdy::native::kDefaultAtomicFileMode) != 0) {
    close(fd);
    std::error_code ec;
    std::filesystem::remove(writable.data(), ec);
    return std::nullopt;
  }

  return StagedDownloadFile{.fd = fd, .path = writable.data()};
}

auto install_staged_download(StagedDownloadFile &staged,
                             const std::filesystem::path &destination) -> bool {
  std::error_code ec;
  std::filesystem::rename(staged.path, destination, ec);
  if (ec) {
    cleanup_staged_download(staged);
    return false;
  }
  howdy::native::sync_parent_directory(destination);
  staged.path.clear();
  return true;
}

auto download_file(const std::string &url, StagedDownloadFile &staged) -> bool {
  CURL *curl = curl_easy_init();
  if (curl == nullptr) {
    return false;
  }

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  configure_transfer_policy(curl);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &staged.fd);
  const CURLcode result = curl_easy_perform(curl);

  long status_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
  curl_easy_cleanup(curl);
  if (result != CURLE_OK || status_code < 200 || status_code >= 400 ||
      fsync(staged.fd) != 0) {
    return false;
  }
  if (close(staged.fd) != 0) {
    staged.fd = -1;
    return false;
  }
  staged.fd = -1;
  return true;
}

}  // namespace

int download_models_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  const auto models_dir = howdy::native::resolve_models_dir();
  std::filesystem::create_directories(models_dir);
  const auto models_dir_security =
      howdy::native::check_secure_root_owned_directory(models_dir,
                                                       "Models directory");
  if (!models_dir_security.ok) {
    std::cout << models_dir_security.error_message << "\n";
    return kExitAbort;
  }

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
    if (std::filesystem::exists(model.destination)) {
      const auto destination_security =
          howdy::native::check_secure_root_owned_file(model.destination,
                                                      "Model file");
      if (!destination_security.ok) {
        curl_global_cleanup();
        std::cout << destination_security.error_message << "\n";
        return kExitAbort;
      }
    }

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
    auto staged = prepare_staged_download(model.destination);
    if (!staged.has_value()) {
      curl_global_cleanup();
      std::cout << "Failed to prepare destination for model: "
                << model.destination.string() << "\n";
      return kExitAbort;
    }

    if (!download_file(model.url, *staged)) {
      cleanup_staged_download(*staged);
      curl_global_cleanup();
      std::cout << "Failed to download model: " << model.url << "\n";
      return kExitAbort;
    }

    if (bad_model_download(staged->path)) {
      cleanup_staged_download(*staged);
      curl_global_cleanup();
      std::cout << "Downloaded file is not an ONNX model: " << model.url << "\n";
      return kExitAbort;
    }

    auto expected_sha256 = model.sha256;
    if (!expected_sha256.has_value()) {
      expected_sha256 = fetch_remote_sha256(model.url);
    }
    if (!expected_sha256.has_value()) {
      cleanup_staged_download(*staged);
      curl_global_cleanup();
      std::cout << "Failed to verify model checksum metadata: " << model.url << "\n";
      return kExitAbort;
    }

    const auto actual_sha256 = file_sha256(staged->path);
    if (!actual_sha256.has_value() ||
        lower_hex(expected_sha256.value()) != lower_hex(actual_sha256.value())) {
      cleanup_staged_download(*staged);
      curl_global_cleanup();
      std::cout << "Checksum mismatch for " << model.name << "\n";
      std::cout << "Expected SHA256: " << expected_sha256.value() << "\n";
      if (actual_sha256.has_value()) {
        std::cout << "Actual SHA256:   " << actual_sha256.value() << "\n";
      }
      return kExitAbort;
    }

    if (!install_staged_download(*staged, model.destination)) {
      curl_global_cleanup();
      std::cout << "Failed to install downloaded model: "
                << model.destination.string() << "\n";
      return kExitAbort;
    }
  }
  curl_global_cleanup();

  std::cout << "OpenCV face models ready in: " << models_dir.string() << "\n";
  return kExitOk;
}
