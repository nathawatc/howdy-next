#ifndef HOWDY_PAM_TESTING
#define HOWDY_PAM_TESTING
#endif

#include "native_prompt_conversation.hpp"

#include <unistd.h>

#include <array>
#include <iostream>
#include <string>

namespace {

auto expect(bool condition, const std::string &message) -> bool {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    return false;
  }
  return true;
}

}  // namespace

auto main() -> int {
  bool ok = true;

  std::array<int, 2> abort_pipe{{-1, -1}};
  ok &= expect(pipe(abort_pipe.data()) == 0, "creates abort pipe");
  if (!ok) {
    return 1;
  }

  {
    SigintAbortHandlerForTesting handler(abort_pipe[1]);
    ok &= expect(handler.installed(), "installs SIGINT abort handler");
    ok &= expect(sigint_abort_handler_ready_for_testing(),
                 "SIGINT bridge reports ready");

    trigger_sigint_abort_for_testing();

    char signal_byte = '\0';
    const ssize_t bytes_read = read(abort_pipe[0], &signal_byte, 1);
    ok &= expect(bytes_read == 1, "SIGINT bridge writes to abort pipe");
    ok &= expect(signal_byte == 'i', "SIGINT bridge writes the interrupt marker");
  }

  ok &= expect(!sigint_abort_handler_ready_for_testing(),
               "SIGINT bridge resets after handler destruction");

  close(abort_pipe[0]);
  close(abort_pipe[1]);

  return ok ? 0 : 1;
}
