#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "common/file_security.hpp"
#include "common/user_names.hpp"
#include "config/config_utils.hpp"
#include "config/runtime_paths.hpp"

namespace {

constexpr std::size_t kCopyBufferSize = 64 * 1024;

struct PreparedPaths {
  std::filesystem::path root_dir;
  std::filesystem::path config_path;
  std::filesystem::path user_models_dir;
};

auto usage(const char *argv0) -> void {
  std::cout << "Usage: " << argv0 << " prepare <user>\n";
}

auto fail(const std::string &message) -> int {
  std::cerr << message << "\n";
  return 1;
}

auto runtime_root_for(uid_t uid) -> std::filesystem::path {
  return std::filesystem::path("/run/user") / std::to_string(uid);
}

auto validate_runtime_root(const std::filesystem::path &path, uid_t uid)
    -> bool {
  struct stat stat_ {};
  if (stat(path.c_str(), &stat_) != 0) {
    std::cerr << "Failed to inspect runtime directory: " << path << " ("
              << std::strerror(errno) << ")\n";
    return false;
  }

  if (!S_ISDIR(stat_.st_mode) || stat_.st_uid != uid ||
      (stat_.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
    std::cerr << "Runtime directory is not a private user directory: " << path
              << "\n";
    return false;
  }

  return true;
}

auto make_private_runtime_dir(uid_t uid, gid_t gid)
    -> std::optional<std::filesystem::path> {
  const auto runtime_root = runtime_root_for(uid);
  if (!validate_runtime_root(runtime_root, uid)) {
    return std::nullopt;
  }

  std::string templ = (runtime_root / "howdy-pam-XXXXXX").string();
  std::vector<char> buffer(templ.begin(), templ.end());
  buffer.push_back('\0');

  char *created = mkdtemp(buffer.data());
  if (created == nullptr) {
    std::cerr << "Failed to create private runtime directory: "
              << std::strerror(errno) << "\n";
    return std::nullopt;
  }

  std::filesystem::path path(created);
  if (chown(path.c_str(), uid, gid) != 0 || chmod(path.c_str(), 0700) != 0) {
    std::cerr << "Failed to secure private runtime directory: "
              << std::strerror(errno) << "\n";
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    return std::nullopt;
  }

  return path;
}

auto secure_source_file_stat(int fd, const std::string &label) -> bool {
  struct stat stat_ {};
  if (fstat(fd, &stat_) != 0) {
    std::cerr << "Failed to inspect " << label << ": " << std::strerror(errno)
              << "\n";
    return false;
  }

  if (!S_ISREG(stat_.st_mode) || stat_.st_uid != 0 ||
      (stat_.st_mode & (S_IWGRP | S_IWOTH)) != 0 || stat_.st_nlink != 1) {
    std::cerr << label << " failed secure file validation\n";
    return false;
  }

  return true;
}

auto write_all(int fd, const char *data, ssize_t size) -> bool {
  ssize_t written = 0;
  while (written < size) {
    const ssize_t result =
        write(fd, data + written, static_cast<std::size_t>(size - written));
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result <= 0) {
      return false;
    }
    written += result;
  }
  return true;
}

auto copy_file_for_user(const std::filesystem::path &source,
                        const std::filesystem::path &destination,
                        const std::string &label, uid_t uid, gid_t gid)
    -> bool {
  const int source_fd = open(source.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (source_fd < 0) {
    std::cerr << "Failed to open " << label << ": " << source << " ("
              << std::strerror(errno) << ")\n";
    return false;
  }

  if (!secure_source_file_stat(source_fd, label)) {
    close(source_fd);
    return false;
  }

  const int destination_fd = open(destination.c_str(),
                                  O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                                  0600);
  if (destination_fd < 0) {
    std::cerr << "Failed to create runtime " << label << ": " << destination
              << " (" << std::strerror(errno) << ")\n";
    close(source_fd);
    return false;
  }

  bool ok = true;
  std::vector<char> buffer(kCopyBufferSize);
  while (true) {
    const ssize_t read_size = read(source_fd, buffer.data(), buffer.size());
    if (read_size < 0 && errno == EINTR) {
      continue;
    }
    if (read_size < 0) {
      ok = false;
      break;
    }
    if (read_size == 0) {
      break;
    }
    if (!write_all(destination_fd, buffer.data(), read_size)) {
      ok = false;
      break;
    }
  }

  if (fchown(destination_fd, uid, gid) != 0 || fchmod(destination_fd, 0600) != 0) {
    ok = false;
  }

  close(destination_fd);
  close(source_fd);
  return ok;
}

auto prepare_for_user(const std::string &user) -> int {
  if (geteuid() != 0) {
    return fail("howdy-auth-helper must be installed setuid root");
  }

  if (!howdy::native::is_valid_model_user_name(user)) {
    return fail(howdy::native::kInvalidUserNameMessage);
  }

  const uid_t uid = getuid();
  const passwd *entry = getpwuid(uid);
  if (entry == nullptr) {
    return fail("Failed to resolve calling user");
  }
  if (entry->pw_name == nullptr || user != entry->pw_name) {
    return fail(
        "howdy-auth-helper can only prepare auth files for the calling user");
  }
  const gid_t gid = entry->pw_gid;

  auto runtime_dir = make_private_runtime_dir(uid, gid);
  if (!runtime_dir.has_value()) {
    return 1;
  }

  PreparedPaths prepared{
      .root_dir = *runtime_dir,
      .config_path = *runtime_dir / "config.ini",
      .user_models_dir = *runtime_dir / "models",
  };

  std::error_code ec;
  std::filesystem::create_directory(prepared.user_models_dir, ec);
  if (ec || chown(prepared.user_models_dir.c_str(), uid, gid) != 0 ||
      chmod(prepared.user_models_dir.c_str(), 0700) != 0) {
    std::cerr << "Failed to create runtime user models directory\n";
    std::filesystem::remove_all(prepared.root_dir, ec);
    return 1;
  }

  const auto source_config = howdy::native::resolve_config_path();
  const auto config_security =
      howdy::native::check_secure_config_path(source_config);
  if (!config_security.ok) {
    std::cerr << config_security.error_message << "\n";
    std::filesystem::remove_all(prepared.root_dir, ec);
    return 1;
  }

  if (!copy_file_for_user(source_config, prepared.config_path, "Config file",
                          uid, gid)) {
    std::filesystem::remove_all(prepared.root_dir, ec);
    return 1;
  }

  const auto source_user_models_dir = howdy::native::resolve_user_models_dir();
  const auto source_model_path =
      howdy::native::resolve_user_model_path(source_user_models_dir, user);
  if (!source_model_path.has_value()) {
    std::filesystem::remove_all(prepared.root_dir, ec);
    return fail(howdy::native::kInvalidUserNameMessage);
  }

  const auto source_models_security =
      howdy::native::check_secure_root_owned_directory_tree(
          source_user_models_dir, "User models directory");
  if (!source_models_security.ok) {
    std::cerr << source_models_security.error_message << "\n";
    std::filesystem::remove_all(prepared.root_dir, ec);
    return 1;
  }

  const bool source_model_exists = std::filesystem::exists(*source_model_path, ec);
  if (ec) {
    std::cerr << "Failed to inspect user model file: " << *source_model_path
              << " (" << ec.message() << ")\n";
    std::filesystem::remove_all(prepared.root_dir, ec);
    return 1;
  }

  if (source_model_exists) {
    const auto source_model_security =
        howdy::native::check_secure_root_owned_file_with_directory(
            *source_model_path, "User models directory", "User model file");
    if (!source_model_security.ok) {
      std::cerr << source_model_security.error_message << "\n";
      std::filesystem::remove_all(prepared.root_dir, ec);
      return 1;
    }

    const auto runtime_model_path =
        prepared.user_models_dir / source_model_path->filename();
    if (!copy_file_for_user(*source_model_path, runtime_model_path,
                            "User model file", uid, gid)) {
      std::filesystem::remove_all(prepared.root_dir, ec);
      return 1;
    }
  }

  std::cout << "CONFIG_PATH=" << prepared.config_path.string() << "\n";
  std::cout << "USER_MODELS_DIR=" << prepared.user_models_dir.string() << "\n";
  return 0;
}

}  // namespace

auto main(int argc, char **argv) -> int {
  if (argc == 2 && (std::string(argv[1]) == "--help" ||
                   std::string(argv[1]) == "-h")) {
    usage(argv[0]);
    return 0;
  }

  if (argc != 3 || std::string(argv[1]) != "prepare") {
    usage(argv[0]);
    return 1;
  }

  return prepare_for_user(argv[2]);
}
