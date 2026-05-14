#include "cli/config_cli.hpp"

#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "config/config_reader.hpp"
#include "config/runtime_paths.hpp"

namespace {

namespace fs = std::filesystem;

constexpr int kExitOk = 0;
constexpr int kExitAbort = 1;

struct InvokingUser {
  uid_t uid = 0;
  gid_t gid = 0;
  std::string name;
};

auto parse_uid_env(const char *value) -> std::optional<uid_t> {
  if (value == nullptr || value[0] == '\0') {
    return std::nullopt;
  }

  errno = 0;
  char *end = nullptr;
  const auto raw_uid = std::strtoul(value, &end, 10);
  if (errno != 0 || end == value || end == nullptr || *end != '\0' ||
      raw_uid > std::numeric_limits<uid_t>::max()) {
    return std::nullopt;
  }

  return static_cast<uid_t>(raw_uid);
}

auto parse_gid_env(const char *value) -> std::optional<gid_t> {
  if (value == nullptr || value[0] == '\0') {
    return std::nullopt;
  }

  errno = 0;
  char *end = nullptr;
  const auto raw_gid = std::strtoul(value, &end, 10);
  if (errno != 0 || end == value || end == nullptr || *end != '\0' ||
      raw_gid > std::numeric_limits<gid_t>::max()) {
    return std::nullopt;
  }

  return static_cast<gid_t>(raw_gid);
}

auto invoking_user_from_pwd(const passwd &pwd, gid_t gid_override)
    -> InvokingUser {
  return InvokingUser{.uid = pwd.pw_uid, .gid = gid_override, .name = pwd.pw_name};
}

auto resolve_invoking_user() -> std::optional<InvokingUser> {
  if (const auto sudo_uid = parse_uid_env(std::getenv("SUDO_UID"))) {
    if (passwd *pwd = getpwuid(*sudo_uid); pwd != nullptr) {
      const auto sudo_gid = parse_gid_env(std::getenv("SUDO_GID")).value_or(pwd->pw_gid);
      return invoking_user_from_pwd(*pwd, sudo_gid);
    }
  }

  if (const char *doas_user = std::getenv("DOAS_USER");
      doas_user != nullptr && doas_user[0] != '\0') {
    if (passwd *pwd = getpwnam(doas_user); pwd != nullptr) {
      return invoking_user_from_pwd(*pwd, pwd->pw_gid);
    }
  }

  if (const auto pkexec_uid = parse_uid_env(std::getenv("PKEXEC_UID"))) {
    if (passwd *pwd = getpwuid(*pkexec_uid); pwd != nullptr) {
      return invoking_user_from_pwd(*pwd, pwd->pw_gid);
    }
  }

  return std::nullopt;
}

auto is_safe_editor_path(const fs::path &path) -> bool {
  return path.is_absolute() && fs::is_regular_file(path) &&
         access(path.c_str(), X_OK) == 0;
}

auto resolve_editor(bool allow_env_editor) -> std::string {
  if (allow_env_editor) {
    if (const char *editor = std::getenv("EDITOR");
        editor != nullptr && editor[0] != '\0') {
      const fs::path editor_path(editor);
      if (is_safe_editor_path(editor_path)) {
        return editor_path.string();
      }
    }
  }

  for (const char *candidate : {"/usr/bin/micro", "/usr/bin/nano", "/usr/bin/vi"}) {
    if (access(candidate, X_OK) == 0) {
      return candidate;
    }
  }

  return {};
}

void remove_if_exists(const fs::path &path) {
  std::error_code ec;
  fs::remove(path, ec);
}

auto create_temp_copy(const fs::path &source_path,
                      const std::optional<InvokingUser> &invoking_user)
    -> std::optional<fs::path> {
  std::ifstream input(source_path, std::ios::binary);
  if (!input.is_open()) {
    return std::nullopt;
  }

  fs::path temp_dir = fs::temp_directory_path();
  std::string temp_template = (temp_dir / "howdy-config-XXXXXX").string();
  std::vector<char> writable(temp_template.begin(), temp_template.end());
  writable.push_back('\0');

  const int fd = mkstemp(writable.data());
  if (fd < 0) {
    return std::nullopt;
  }

  const fs::path temp_path(writable.data());
  bool ok = true;

  if (fchmod(fd, S_IRUSR | S_IWUSR) != 0) {
    ok = false;
  }

  if (ok && invoking_user.has_value() &&
      fchown(fd, invoking_user->uid, invoking_user->gid) != 0) {
    ok = false;
  }

  if (ok) {
    std::array<char, 8192> buffer{};
    while (input.good()) {
      input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
      const auto bytes_read = input.gcount();
      if (bytes_read <= 0) {
        continue;
      }

      const char *cursor = buffer.data();
      std::size_t remaining = static_cast<std::size_t>(bytes_read);
      while (remaining > 0) {
        const auto written = write(fd, cursor, remaining);
        if (written < 0) {
          if (errno == EINTR) {
            continue;
          }
          ok = false;
          break;
        }
        cursor += written;
        remaining -= static_cast<std::size_t>(written);
      }

      if (!ok) {
        break;
      }
    }
  }

  if (ok && fsync(fd) != 0) {
    ok = false;
  }

  close(fd);

  if (!ok || (!input.good() && !input.eof())) {
    remove_if_exists(temp_path);
    return std::nullopt;
  }

  return temp_path;
}

auto run_editor(const std::string &editor, const fs::path &temp_path,
                const std::optional<InvokingUser> &invoking_user) -> int {
  const pid_t child_pid = fork();
  if (child_pid < 0) {
    return -1;
  }

  if (child_pid == 0) {
    if (invoking_user.has_value()) {
      if (initgroups(invoking_user->name.c_str(), invoking_user->gid) != 0 ||
          setgid(invoking_user->gid) != 0 || setuid(invoking_user->uid) != 0) {
        _exit(126);
      }
    }

    char *const exec_argv[] = {
        const_cast<char *>(editor.c_str()),
        const_cast<char *>(temp_path.c_str()),
        nullptr,
    };
    execv(editor.c_str(), exec_argv);
    _exit(127);
  }

  int status = 0;
  while (waitpid(child_pid, &status, 0) < 0) {
    if (errno != EINTR) {
      return -1;
    }
  }
  return status;
}

auto copy_file_contents(int input_fd, int output_fd) -> bool {
  std::array<char, 8192> buffer{};
  while (true) {
    const auto bytes_read = read(input_fd, buffer.data(), buffer.size());
    if (bytes_read == 0) {
      return true;
    }
    if (bytes_read < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }

    const char *cursor = buffer.data();
    std::size_t remaining = static_cast<std::size_t>(bytes_read);
    while (remaining > 0) {
      const auto bytes_written = write(output_fd, cursor, remaining);
      if (bytes_written < 0) {
        if (errno == EINTR) {
          continue;
        }
        return false;
      }
      cursor += bytes_written;
      remaining -= static_cast<std::size_t>(bytes_written);
    }
  }
}

auto files_match(const fs::path &left_path, const fs::path &right_path) -> bool {
  std::ifstream left(left_path, std::ios::binary);
  std::ifstream right(right_path, std::ios::binary);
  if (!left.is_open() || !right.is_open()) {
    return false;
  }

  std::array<char, 8192> left_buffer{};
  std::array<char, 8192> right_buffer{};
  while (true) {
    left.read(left_buffer.data(), static_cast<std::streamsize>(left_buffer.size()));
    right.read(right_buffer.data(), static_cast<std::streamsize>(right_buffer.size()));

    const auto left_count = left.gcount();
    const auto right_count = right.gcount();
    if (left_count != right_count) {
      return false;
    }

    if (left_count == 0) {
      return true;
    }

    if (!std::equal(left_buffer.begin(), left_buffer.begin() + left_count,
                    right_buffer.begin())) {
      return false;
    }

    if ((!left.good() && !left.eof()) || (!right.good() && !right.eof())) {
      return false;
    }
  }
}

auto replace_config_from_temp(const fs::path &config_path, const fs::path &temp_path)
    -> bool {
  struct stat current_stat {};
  if (stat(config_path.c_str(), &current_stat) != 0) {
    return false;
  }

  const int input_fd = open(temp_path.c_str(), O_RDONLY | O_NOFOLLOW);
  if (input_fd < 0) {
    return false;
  }

  struct stat edited_stat {};
  const bool edited_ok =
      fstat(input_fd, &edited_stat) == 0 && S_ISREG(edited_stat.st_mode);
  if (!edited_ok) {
    close(input_fd);
    return false;
  }

  std::string output_template =
      (config_path.parent_path() / ".howdy-config-XXXXXX").string();
  std::vector<char> writable(output_template.begin(), output_template.end());
  writable.push_back('\0');

  const int output_fd = mkstemp(writable.data());
  if (output_fd < 0) {
    close(input_fd);
    return false;
  }

  const fs::path staged_path(writable.data());
  bool ok = true;
  if (fchmod(output_fd, current_stat.st_mode & 07777) != 0 ||
      fchown(output_fd, current_stat.st_uid, current_stat.st_gid) != 0) {
    ok = false;
  }

  if (ok && !copy_file_contents(input_fd, output_fd)) {
    ok = false;
  }

  if (ok && fsync(output_fd) != 0) {
    ok = false;
  }

  close(input_fd);
  close(output_fd);

  if (!ok) {
    remove_if_exists(staged_path);
    return false;
  }

  std::error_code ec;
  fs::rename(staged_path, config_path, ec);
  if (ec) {
    remove_if_exists(staged_path);
    return false;
  }

  const int dir_fd = open(config_path.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
  if (dir_fd >= 0) {
    fsync(dir_fd);
    close(dir_fd);
  }

  return true;
}

}  // namespace

int config_main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  const auto invoking_user = resolve_invoking_user();
  const auto editor = resolve_editor(invoking_user.has_value());
  if (editor.empty()) {
    std::cout << "Error: Could not find a suitable text editor.\n";
    std::cout << "Set EDITOR to an absolute executable path, or install one of: micro, nano, vi.\n";
    return kExitAbort;
  }

  const auto config_path = howdy::native::resolve_config_path();
  const auto temp_path = create_temp_copy(config_path, invoking_user);
  if (!temp_path) {
    std::cout << "Failed to prepare a temporary config copy\n";
    return kExitAbort;
  }

  std::cout << "Editing config.ini in "
            << fs::path(editor).filename().string() << "\n";

  const int status = run_editor(editor, *temp_path, invoking_user);
  if (status < 0) {
    remove_if_exists(*temp_path);
    std::cout << "Failed to launch editor\n";
    return kExitAbort;
  }

  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    remove_if_exists(*temp_path);
    std::cout << "Editor exited unsuccessfully; config not updated\n";
    return kExitAbort;
  }

  howdy::native::ConfigReader edited_config(temp_path->string());
  if (!edited_config.ok()) {
    std::cout << "Edited config is invalid and was not installed: " << *temp_path
              << "\n";
    return kExitAbort;
  }

  if (files_match(config_path, *temp_path)) {
    remove_if_exists(*temp_path);
    std::cout << "No config changes made\n";
    return kExitOk;
  }

  if (!replace_config_from_temp(config_path, *temp_path)) {
    remove_if_exists(*temp_path);
    std::cout << "Failed to install edited config\n";
    return kExitAbort;
  }

  remove_if_exists(*temp_path);
  std::cout << "Config updated\n";
  return kExitOk;
}
