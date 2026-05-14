#include "main.hpp"

#include <iostream>
#include <security/pam_modules.h>

namespace {

int identify_calls = 0;
bool last_ask_auth_tok = false;

auto expect(bool condition, const char *message) -> bool {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    return false;
  }
  return true;
}

}  // namespace

auto identify(pam_handle_t * /*pamh*/, int /*flags*/, int /*argc*/,
              const char ** /*argv*/, bool ask_auth_tok) -> int {
  ++identify_calls;
  last_ask_auth_tok = ask_auth_tok;
  return ask_auth_tok ? PAM_SUCCESS : PAM_AUTH_ERR;
}

auto main() -> int {
  bool ok = true;

  unsetenv("SSH_CONNECTION");
  setenv("SSH_CONNECTION_EXTRA", "1", 1);
  ok &= expect(!checkenv("SSH_CONNECTION"),
               "checkenv ignores prefixed variable names");
  unsetenv("SSH_CONNECTION_EXTRA");

  identify_calls = 0;
  last_ask_auth_tok = false;
  ok &= expect(pam_sm_authenticate(nullptr, 0, 0, nullptr) == PAM_SUCCESS,
               "authenticate forwards to identify");
  ok &= expect(identify_calls == 1, "authenticate calls identify once");
  ok &= expect(last_ask_auth_tok, "authenticate requests auth token");

  identify_calls = 0;
  last_ask_auth_tok = true;
  ok &= expect(pam_sm_open_session(nullptr, 0, 0, nullptr) == PAM_IGNORE,
               "open_session ignores auth");
  ok &= expect(identify_calls == 0, "open_session does not call identify");

  return ok ? 0 : 1;
}
