#include "main.hpp"

#include <iostream>
#include <stdexcept>

#include <security/pam_modules.h>

namespace {

    int  identify_calls      = 0;
    bool last_ask_auth_tok   = false;
    int  identify_throw_mode = 0;

    auto expect(bool condition, const char *message) -> bool {
        if (!condition) {
            std::cerr << "FAIL: " << message << "\n";
            return false;
        }
        return true;
    }

}  // namespace

auto identify(pam_handle_t * /*pamh*/, int /*flags*/, int /*argc*/, const char ** /*argv*/,
              bool ask_auth_tok) -> int {
    ++identify_calls;
    last_ask_auth_tok = ask_auth_tok;
    if (identify_throw_mode == 1) {
        throw std::runtime_error("simulated identify failure");
    }
    if (identify_throw_mode == 2) {
        throw 1;
    }
    return ask_auth_tok ? PAM_SUCCESS : PAM_AUTH_ERR;
}

auto main() -> int {
    bool ok = true;

    unsetenv("SSH_CONNECTION");
    setenv("SSH_CONNECTION_EXTRA", "1", 1);
    ok &= expect(!checkenv("SSH_CONNECTION"), "checkenv ignores prefixed variable names");
    unsetenv("SSH_CONNECTION_EXTRA");

    identify_calls    = 0;
    last_ask_auth_tok = false;
    ok &= expect(pam_sm_authenticate(nullptr, 0, 0, nullptr) == PAM_SUCCESS,
                 "authenticate forwards to identify");
    ok &= expect(identify_calls == 1, "authenticate calls identify once");
    ok &= expect(last_ask_auth_tok, "authenticate requests auth token");

    identify_calls      = 0;
    identify_throw_mode = 1;
    ok &= expect(pam_sm_authenticate(nullptr, 0, 0, nullptr) == PAM_AUTH_ERR,
                 "authenticate fails closed on std exception");
    ok &= expect(identify_calls == 1, "std exception path still calls identify once");
    identify_throw_mode = 0;

    identify_calls      = 0;
    identify_throw_mode = 2;
    ok &= expect(pam_sm_authenticate(nullptr, 0, 0, nullptr) == PAM_AUTH_ERR,
                 "authenticate fails closed on unknown exception");
    ok &= expect(identify_calls == 1, "unknown exception path still calls identify once");
    identify_throw_mode = 0;

    identify_calls    = 0;
    last_ask_auth_tok = true;
    ok &= expect(pam_sm_open_session(nullptr, 0, 0, nullptr) == PAM_IGNORE,
                 "open_session ignores auth");
    ok &= expect(pam_sm_acct_mgmt(nullptr, 0, 0, nullptr) == PAM_IGNORE, "acct_mgmt ignores auth");
    ok &= expect(pam_sm_close_session(nullptr, 0, 0, nullptr) == PAM_IGNORE,
                 "close_session ignores auth");
    ok &= expect(pam_sm_chauthtok(nullptr, 0, 0, nullptr) == PAM_IGNORE, "chauthtok ignores auth");
    ok &= expect(pam_sm_setcred(nullptr, 0, 0, nullptr) == PAM_IGNORE, "setcred ignores auth");
    ok &= expect(identify_calls == 0, "non-auth PAM entrypoints do not call identify");

    return ok ? 0 : 1;
}
