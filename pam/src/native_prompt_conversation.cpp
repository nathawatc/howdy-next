#include "native_prompt_conversation.hpp"

#include <fcntl.h>
#include <poll.h>
#include <security/pam_appl.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <termios.h>

namespace {

auto open_tty_fd(pam_handle_t *pamh) -> int {
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

void close_fd(int &fd) {
  if (fd >= 0) {
    close(fd);
    fd = -1;
  }
}

auto set_nonblocking(int fd) -> bool {
  const int flags = fcntl(fd, F_GETFL);
  if (flags < 0) {
    return false;
  }
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

auto write_all(int fd, const std::string &text) -> bool {
  const char *cursor = text.c_str();
  std::size_t remaining = text.size();
  while (remaining > 0) {
    const ssize_t bytes_written = write(fd, cursor, remaining);
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

auto write_newline(int fd) -> void {
  constexpr char kNewline = '\n';
  while (write(fd, &kNewline, 1) < 0 && errno == EINTR) {
  }
}

void drain_abort_pipe(int fd) {
  std::array<char, 32> buffer{};
  while (read(fd, buffer.data(), buffer.size()) > 0) {
  }
}

}  // namespace

NativePromptConversation::NativePromptConversation(pam_handle_t *pamh)
    : pamh_(pamh), override_conv_{dispatch, this} {
  const void *conv_ptr = nullptr;
  if (pam_get_item(pamh_, PAM_CONV, &conv_ptr) != PAM_SUCCESS ||
      conv_ptr == nullptr) {
    return;
  }

  original_conv_ = *static_cast<const struct pam_conv *>(conv_ptr);
  has_original_conv_ = original_conv_.conv != nullptr;
  if (!has_original_conv_) {
    return;
  }

  tty_fd_ = open_tty_fd(pamh_);
  if (tty_fd_ < 0) {
    return;
  }

  if (pipe(abort_pipe_.data()) != 0) {
    close_fd(tty_fd_);
    return;
  }

  if (!set_nonblocking(abort_pipe_[0]) || !set_nonblocking(abort_pipe_[1])) {
    close_fd(tty_fd_);
    close_fd(abort_pipe_[0]);
    close_fd(abort_pipe_[1]);
  }
}

NativePromptConversation::~NativePromptConversation() {
  if (installed_) {
    (void)pam_set_item(pamh_, PAM_CONV, &original_conv_);
  }

  close_fd(tty_fd_);
  close_fd(abort_pipe_[0]);
  close_fd(abort_pipe_[1]);
}

auto NativePromptConversation::available() const -> bool {
  return has_original_conv_ && tty_fd_ >= 0 && abort_pipe_[0] >= 0 &&
         abort_pipe_[1] >= 0;
}

auto NativePromptConversation::install() -> int {
  if (!available()) {
    return PAM_SYSTEM_ERR;
  }

  const int pam_res = pam_set_item(pamh_, PAM_CONV, &override_conv_);
  if (pam_res == PAM_SUCCESS) {
    installed_ = true;
  }
  return pam_res;
}

void NativePromptConversation::request_abort() {
  abort_requested_.store(true);
  if (abort_pipe_[1] < 0) {
    return;
  }

  constexpr char kSignal = 'x';
  if (write(abort_pipe_[1], &kSignal, 1) < 0 && errno != EAGAIN &&
      errno != EWOULDBLOCK && errno != EINTR) {
  }
}

auto NativePromptConversation::dispatch(int num_msg,
                                        const struct pam_message **msgm,
                                        struct pam_response **response,
                                        void *appdata_ptr) -> int {
  auto *self = static_cast<NativePromptConversation *>(appdata_ptr);
  if (self == nullptr) {
    return PAM_CONV_ERR;
  }
  return self->handle(num_msg, msgm, response);
}

auto NativePromptConversation::handle(int num_msg,
                                      const struct pam_message **msgm,
                                      struct pam_response **response) -> int {
  if (num_msg == 1 && msgm != nullptr && msgm[0] != nullptr &&
      msgm[0]->msg_style == PAM_PROMPT_ECHO_OFF) {
    return prompt_hidden_password(*msgm[0], response);
  }

  return delegate(num_msg, msgm, response);
}

auto NativePromptConversation::delegate(int num_msg,
                                        const struct pam_message **msgm,
                                        struct pam_response **response) const
    -> int {
  if (!has_original_conv_) {
    return PAM_CONV_ERR;
  }
  return original_conv_.conv(num_msg, msgm, response, original_conv_.appdata_ptr);
}

auto NativePromptConversation::prompt_hidden_password(
    const struct pam_message &message, struct pam_response **response) -> int {
  if (response == nullptr || tty_fd_ < 0) {
    return PAM_CONV_ERR;
  }

  if (abort_requested_.load()) {
    return PAM_CONV_ERR;
  }

  struct termios original_termios {};
  if (tcgetattr(tty_fd_, &original_termios) != 0) {
    return PAM_CONV_ERR;
  }

  struct termios prompt_termios = original_termios;
  prompt_termios.c_lflag &= static_cast<tcflag_t>(~ECHO);
  prompt_termios.c_lflag &= static_cast<tcflag_t>(~ICANON);
  prompt_termios.c_cc[VMIN] = 1;
  prompt_termios.c_cc[VTIME] = 0;
  if (tcsetattr(tty_fd_, TCSANOW, &prompt_termios) != 0) {
    return PAM_CONV_ERR;
  }

  const std::string prompt_text = message.msg == nullptr ? "" : message.msg;
  if (!write_all(tty_fd_, prompt_text)) {
    (void)tcsetattr(tty_fd_, TCSANOW, &original_termios);
    return PAM_CONV_ERR;
  }

  std::string password;
  std::array<struct pollfd, 2> fds{{
      {tty_fd_, POLLIN, 0},
      {abort_pipe_[0], POLLIN, 0},
  }};

  while (true) {
    const int poll_result = poll(fds.data(), fds.size(), -1);
    if (poll_result < 0) {
      if (errno == EINTR) {
        continue;
      }
      (void)tcsetattr(tty_fd_, TCSANOW, &original_termios);
      return PAM_CONV_ERR;
    }

    if ((fds[1].revents & POLLIN) != 0 || abort_requested_.load()) {
      drain_abort_pipe(abort_pipe_[0]);
      (void)tcsetattr(tty_fd_, TCSANOW, &original_termios);
      write_newline(tty_fd_);
      return PAM_CONV_ERR;
    }

    if ((fds[0].revents & POLLIN) == 0) {
      continue;
    }

    char ch = '\0';
    const ssize_t bytes_read = read(tty_fd_, &ch, 1);
    if (bytes_read < 0) {
      if (errno == EINTR) {
        continue;
      }
      (void)tcsetattr(tty_fd_, TCSANOW, &original_termios);
      return PAM_CONV_ERR;
    }
    if (bytes_read == 0) {
      continue;
    }

    if (ch == '\n' || ch == '\r') {
      break;
    }

    if (ch == '\b' || ch == 127) {
      if (!password.empty()) {
        password.pop_back();
      }
      continue;
    }

    password.push_back(ch);
  }

  (void)tcsetattr(tty_fd_, TCSANOW, &original_termios);
  write_newline(tty_fd_);

  auto *pam_response =
      static_cast<struct pam_response *>(calloc(1, sizeof(struct pam_response)));
  if (pam_response == nullptr) {
    return PAM_BUF_ERR;
  }

  pam_response->resp =
      static_cast<char *>(calloc(password.size() + 1, sizeof(char)));
  if (pam_response->resp == nullptr) {
    free(pam_response);
    return PAM_BUF_ERR;
  }

  std::memcpy(pam_response->resp, password.c_str(), password.size());
  pam_response->resp_retcode = 0;
  *response = pam_response;
  return PAM_SUCCESS;
}
