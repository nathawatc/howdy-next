#pragma once

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <cerrno>
#include <filesystem>
#include <optional>
#include <utility>

namespace howdy::native {

struct ScopedFileLock {
  int fd = -1;
  std::filesystem::path path;

  ScopedFileLock() = default;
  ScopedFileLock(const ScopedFileLock &) = delete;
  auto operator=(const ScopedFileLock &) -> ScopedFileLock & = delete;
  ScopedFileLock(ScopedFileLock &&other) noexcept
      : fd(other.fd), path(std::move(other.path)) {
    other.fd = -1;
  }
  auto operator=(ScopedFileLock &&other) noexcept -> ScopedFileLock & {
    if (this != &other) {
      release();
      fd = other.fd;
      path = std::move(other.path);
      other.fd = -1;
    }
    return *this;
  }
  ~ScopedFileLock() { release(); }

  void release() {
    if (fd < 0) {
      return;
    }
    while (flock(fd, LOCK_UN) != 0 && errno == EINTR) {
    }
    close(fd);
    fd = -1;
  }
};

inline auto lock_file_path(const std::filesystem::path &target_path)
    -> std::filesystem::path {
  return target_path.string() + ".lock";
}

inline auto acquire_file_lock(const std::filesystem::path &target_path)
    -> std::optional<ScopedFileLock> {
  std::error_code ec;
  std::filesystem::create_directories(target_path.parent_path(), ec);
  if (ec) {
    return std::nullopt;
  }

  const auto path = lock_file_path(target_path);
  const int fd =
      open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
           S_IRUSR | S_IWUSR);
  if (fd < 0) {
    return std::nullopt;
  }

  while (flock(fd, LOCK_EX) != 0) {
    if (errno != EINTR) {
      close(fd);
      return std::nullopt;
    }
  }

  ScopedFileLock lock;
  lock.fd = fd;
  lock.path = path;
  return lock;
}

}  // namespace howdy::native
