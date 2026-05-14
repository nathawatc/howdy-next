#ifndef HOWDY_PAM_TESTING
#define HOWDY_PAM_TESTING
#endif

#define private public
#include "native_prompt_conversation.hpp"
#undef private

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#include <array>
#include <iostream>
#include <string>
#include <thread>

namespace {

auto expect(bool condition, const std::string &message) -> bool {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    return false;
  }
  return true;
}

auto open_pty_pair(int *master_fd, int *slave_fd) -> bool {
  *master_fd = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
  if (*master_fd < 0) {
    return false;
  }

  if (grantpt(*master_fd) != 0 || unlockpt(*master_fd) != 0) {
    close(*master_fd);
    *master_fd = -1;
    return false;
  }

  char *slave_name = ptsname(*master_fd);
  if (slave_name == nullptr) {
    close(*master_fd);
    *master_fd = -1;
    return false;
  }

  *slave_fd = open(slave_name, O_RDWR | O_NOCTTY | O_CLOEXEC);
  if (*slave_fd < 0) {
    close(*master_fd);
    *master_fd = -1;
    return false;
  }

  return true;
}

}  // namespace

auto main() -> int {
  bool ok = true;

  int master_fd = -1;
  int slave_fd = -1;
  std::array<int, 2> abort_pipe{{-1, -1}};

  ok &= expect(open_pty_pair(&master_fd, &slave_fd), "opens pseudo terminal");
  ok &= expect(pipe(abort_pipe.data()) == 0, "creates abort pipe");
  if (!ok) {
    if (master_fd >= 0) {
      close(master_fd);
    }
    if (slave_fd >= 0) {
      close(slave_fd);
    }
    if (abort_pipe[0] >= 0) {
      close(abort_pipe[0]);
    }
    if (abort_pipe[1] >= 0) {
      close(abort_pipe[1]);
    }
    return 1;
  }

  const struct pam_message message = {
      .msg_style = PAM_PROMPT_ECHO_OFF,
      .msg = "Password: ",
  };

  NativePromptConversation conversation(slave_fd, abort_pipe[0], abort_pipe[1]);

  int prompt_result = PAM_SUCCESS;
  char *response = nullptr;
  std::thread prompt_thread([&] {
    prompt_result = conversation.prompt_input(message, &response, true);
  });

  std::array<char, 64> prompt_buffer{};
  const ssize_t prompt_bytes = read(master_fd, prompt_buffer.data(),
                                    static_cast<size_t>(prompt_buffer.size()));
  ok &= expect(prompt_bytes > 0, "prompt is written to tty");

  constexpr char kCtrlC = 3;
  ok &= expect(write(master_fd, &kCtrlC, 1) == 1,
               "writes Ctrl-C byte to pseudo terminal");

  prompt_thread.join();

  ok &= expect(prompt_result == PAM_CONV_ERR,
               "Ctrl-C byte aborts the native prompt");
  ok &= expect(response == nullptr, "aborted prompt does not return a response");

  struct termios restored_termios {};
  ok &= expect(tcgetattr(slave_fd, &restored_termios) == 0,
               "terminal state remains readable after abort");
  ok &= expect((restored_termios.c_lflag & ICANON) != 0,
               "canonical mode restored after abort");
  ok &= expect((restored_termios.c_lflag & ECHO) != 0,
               "echo restored after abort");
  ok &= expect((restored_termios.c_lflag & ISIG) != 0,
               "signal generation restored after abort");

  if (response != nullptr) {
    std::free(response);
  }
  close(master_fd);

  return ok ? 0 : 1;
}
