#include "tty_restore.hh"

#include <fcntl.h>
#include <security/pam_appl.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <string>

namespace {

auto open_tty_fd(pam_handle *pamh) -> int {
  std::array<std::string, 2> candidates{};
  std::size_t candidate_count = 0;

  const void *tty_item = nullptr;
  if (pam_get_item(pamh, PAM_TTY, &tty_item) == PAM_SUCCESS &&
      tty_item != nullptr) {
    auto tty_path = std::string(static_cast<const char *>(tty_item));
    if (!tty_path.empty()) {
      if (tty_path.front() != '/') {
        tty_path = "/dev/" + tty_path;
      }
      candidates[candidate_count++] = std::move(tty_path);
    }
  }

  candidates[candidate_count++] = "/dev/tty";

  for (std::size_t i = 0; i < candidate_count; ++i) {
    const int fd = open(candidates[i].c_str(), O_RDWR | O_CLOEXEC | O_NOCTTY);
    if (fd >= 0) {
      return fd;
    }
  }

  return -1;
}

}  // namespace

TtyRestoreContext::TtyRestoreContext(pam_handle *pamh) {
  fd_ = open_tty_fd(pamh);
  if (fd_ < 0) {
    return;
  }

  if (tcgetattr(fd_, &original_) != 0) {
    close(fd_);
    fd_ = -1;
    return;
  }

  valid_ = true;
}

TtyRestoreContext::~TtyRestoreContext() {
  if (fd_ >= 0) {
    close(fd_);
  }
}

TtyRestoreContext::TtyRestoreContext(TtyRestoreContext &&other) noexcept
    : fd_(other.fd_), original_(other.original_), valid_(other.valid_) {
  other.fd_ = -1;
  other.valid_ = false;
}

auto TtyRestoreContext::operator=(TtyRestoreContext &&other) noexcept
    -> TtyRestoreContext & {
  if (this == &other) {
    return *this;
  }

  if (fd_ >= 0) {
    close(fd_);
  }

  fd_ = other.fd_;
  original_ = other.original_;
  valid_ = other.valid_;
  other.fd_ = -1;
  other.valid_ = false;
  return *this;
}

auto TtyRestoreContext::can_restore() const -> bool {
  return valid_;
}

auto TtyRestoreContext::restore_echo(std::string *error_message) const -> bool {
  if (!valid_) {
    return true;
  }

  struct termios current {};
  if (tcgetattr(fd_, &current) != 0) {
    if (error_message != nullptr) {
      *error_message =
          "Failed to read terminal state: " + std::string(std::strerror(errno));
    }
    return false;
  }

  if ((current.c_lflag & ECHO) != 0 || (original_.c_lflag & ECHO) == 0) {
    return true;
  }

  if (tcsetattr(fd_, TCSANOW, &original_) != 0) {
    if (error_message != nullptr) {
      *error_message = "Failed to restore terminal echo: " +
                       std::string(std::strerror(errno));
    }
    return false;
  }

  return true;
}

auto TtyRestoreContext::write_newline(std::string *error_message) const -> bool {
  if (!valid_) {
    return true;
  }

  constexpr char kNewline[] = "\n";
  const ssize_t bytes_written = write(fd_, kNewline, sizeof(kNewline) - 1);
  if (bytes_written == static_cast<ssize_t>(sizeof(kNewline) - 1)) {
    return true;
  }

  if (error_message != nullptr) {
    if (bytes_written < 0) {
      *error_message = "Failed to write terminal newline: " +
                       std::string(std::strerror(errno));
    } else {
      *error_message = "Failed to write terminal newline";
    }
  }

  return false;
}
