#include "config/config_utils.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <filesystem>
#include <string_view>
#include <string>
#include <vector>

#include "common/file_security.hpp"

namespace howdy::native {

namespace {

constexpr mode_t kDefaultConfigMode =
    S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;

auto lock_path_for_config(const std::filesystem::path &config_path)
    -> std::filesystem::path {
  return config_path.string() + ".lock";
}

auto open_lock_file(const std::filesystem::path &config_path) -> int {
  return open(lock_path_for_config(config_path).c_str(),
              O_RDWR | O_CREAT | O_CLOEXEC, S_IRUSR | S_IWUSR);
}

auto lock_fd(int fd) -> bool {
  while (flock(fd, LOCK_EX) != 0) {
    if (errno != EINTR) {
      return false;
    }
  }
  return true;
}

void unlock_fd(int fd) {
  while (flock(fd, LOCK_UN) != 0 && errno == EINTR) {
  }
}

auto read_all_from_fd(int fd) -> std::string {
  if (lseek(fd, 0, SEEK_SET) < 0) {
    return {};
  }

  std::string content;
  std::array<char, 4096> buffer{};
  while (true) {
    const auto bytes_read = read(fd, buffer.data(), buffer.size());
    if (bytes_read == 0) {
      return content;
    }
    if (bytes_read < 0) {
      if (errno == EINTR) {
        continue;
      }
      return {};
    }
    content.append(buffer.data(), static_cast<std::size_t>(bytes_read));
  }
}

auto split_lines_preserve_newlines(const std::string &content)
    -> std::vector<std::string> {
  std::vector<std::string> lines;
  std::size_t start = 0;
  while (start < content.size()) {
    const auto end = content.find('\n', start);
    if (end == std::string::npos) {
      lines.push_back(content.substr(start));
      break;
    }
    lines.push_back(content.substr(start, (end - start) + 1));
    start = end + 1;
  }
  return lines;
}

auto write_all_to_fd(int fd, const std::string &content) -> bool {
  const char *cursor = content.data();
  std::size_t remaining = content.size();
  while (remaining > 0) {
    const auto bytes_written = write(fd, cursor, remaining);
    if (bytes_written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    cursor += bytes_written;
    remaining -= static_cast<std::size_t>(bytes_written);
  }
  return true;
}

auto join_lines(const std::vector<std::string> &lines) -> std::string {
  std::string content;
  for (const auto &line : lines) {
    content += line;
  }
  return content;
}

auto sync_parent_directory(const std::filesystem::path &path) -> void {
  const int dir_fd = open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
  if (dir_fd >= 0) {
    fsync(dir_fd);
    close(dir_fd);
  }
}

}  // namespace

auto is_safe_ini_scalar_value(std::string_view value) -> bool {
  if (!value.empty() && value.front() == '[') {
    return false;
  }

  for (const char ch : value) {
    if (ch == '\0' || ch == '\n' || ch == '\r') {
      return false;
    }
  }

  return true;
}

auto read_config_lines(const std::filesystem::path &config_path, bool lock)
    -> std::vector<std::string> {
  std::vector<std::string> lines;

  int lock_fd_handle = -1;
  if (lock) {
    lock_fd_handle = open_lock_file(config_path);
    if (lock_fd_handle < 0 || !lock_fd(lock_fd_handle)) {
      if (lock_fd_handle >= 0) {
        close(lock_fd_handle);
      }
      return lines;
    }
  }

  const int fd = open(config_path.c_str(), O_RDONLY | O_NOFOLLOW);
  if (fd < 0) {
    if (lock_fd_handle >= 0) {
      unlock_fd(lock_fd_handle);
      close(lock_fd_handle);
    }
    return lines;
  }

  const auto security =
      check_secure_root_owned_file(config_path, "Config file");
  if (!security.ok) {
    close(fd);
    if (lock_fd_handle >= 0) {
      unlock_fd(lock_fd_handle);
      close(lock_fd_handle);
    }
    return lines;
  }

  lines = split_lines_preserve_newlines(read_all_from_fd(fd));
  close(fd);
  if (lock_fd_handle >= 0) {
    unlock_fd(lock_fd_handle);
    close(lock_fd_handle);
  }
  return lines;
}

auto atomic_write_lines(const std::filesystem::path &config_path,
                        const std::vector<std::string> &lines) -> bool {
  const auto parent = config_path.parent_path();
  std::filesystem::create_directories(parent);

  struct stat current_stat {};
  const bool have_current_stat = lstat(config_path.c_str(), &current_stat) == 0;
  if (have_current_stat && !S_ISREG(current_stat.st_mode)) {
    return false;
  }

  std::string temp = (parent / ".howdy-config-XXXXXX").string();
  std::vector<char> writable(temp.begin(), temp.end());
  writable.push_back('\0');

  const int fd = mkstemp(writable.data());
  if (fd < 0) {
    return false;
  }

  const std::filesystem::path temp_path(writable.data());
  bool ok = true;
  if (have_current_stat) {
    if (fchmod(fd, current_stat.st_mode & 07777) != 0 ||
        fchown(fd, current_stat.st_uid, current_stat.st_gid) != 0) {
      ok = false;
    }
  } else if (fchmod(fd, kDefaultConfigMode) != 0) {
    ok = false;
  }

  const auto content = join_lines(lines);
  if (ok && !write_all_to_fd(fd, content)) {
    ok = false;
  }

  if (ok && fsync(fd) != 0) {
    ok = false;
  }
  close(fd);

  if (!ok) {
    std::error_code ec;
    std::filesystem::remove(temp_path, ec);
    return false;
  }

  std::error_code ec;
  std::filesystem::rename(temp_path, config_path, ec);
  if (ec) {
    std::filesystem::remove(temp_path, ec);
    return false;
  }
  sync_parent_directory(config_path);
  return true;
}

auto update_config_value(const std::filesystem::path &config_path,
                         const std::string &key, const std::string &value,
                         bool lock) -> bool {
  if (!is_safe_ini_scalar_value(value)) {
    return false;
  }

  const auto security =
      check_secure_root_owned_file(config_path, "Config file");
  if (!security.ok) {
    return false;
  }

  int lock_fd_handle = -1;
  if (lock) {
    lock_fd_handle = open_lock_file(config_path);
    if (lock_fd_handle < 0 || !lock_fd(lock_fd_handle)) {
      if (lock_fd_handle >= 0) {
        close(lock_fd_handle);
      }
      return false;
    }
  }

  const int fd = open(config_path.c_str(), O_RDONLY | O_NOFOLLOW);
  if (fd < 0) {
    if (lock_fd_handle >= 0) {
      unlock_fd(lock_fd_handle);
      close(lock_fd_handle);
    }
    return false;
  }

  auto lines = split_lines_preserve_newlines(read_all_from_fd(fd));
  close(fd);

  bool updated = false;
  for (auto &line : lines) {
    const auto stripped_pos = line.find_first_not_of(" \t");
    if (stripped_pos == std::string::npos) {
      continue;
    }

    const auto stripped = line.substr(stripped_pos);
    if (stripped.rfind(key + " =", 0) == 0 || stripped.rfind(key + " ", 0) == 0) {
      line = key;
      line += " = ";
      line += value;
      line += "\n";
      updated = true;
      break;
    }
  }

  const bool ok = updated && atomic_write_lines(config_path, lines);
  if (lock_fd_handle >= 0) {
    unlock_fd(lock_fd_handle);
    close(lock_fd_handle);
  }
  return ok;
}

}  // namespace howdy::native
