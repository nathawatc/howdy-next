#include "main.hpp"

#include <iostream>
#include <stdexcept>

#include <security/pam_modules.h>

namespace {

	int  identify_calls      = 0;
	bool last_ask_auth_tok   = false;
	int  identify_result     = PAM_SUCCESS;
	int  identify_throw_mode = 0;

	auto reset_identify_state() -> void {
		identify_calls      = 0;
		last_ask_auth_tok   = false;
		identify_result     = PAM_SUCCESS;
		identify_throw_mode = 0;
	}

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
	return identify_result;
}

auto main() -> int {
	bool ok = true;

	setenv("HOWDY_TEST_PRESENT", "1", 1);
	ok &= expect(checkenv("HOWDY_TEST_PRESENT"), "checkenv detects present variable");
	unsetenv("HOWDY_TEST_PRESENT");
	ok &= expect(!checkenv("HOWDY_TEST_PRESENT"), "checkenv rejects absent variable");

	unsetenv("SSH_CONNECTION");
	setenv("SSH_CONNECTION_EXTRA", "1", 1);
	ok &= expect(!checkenv("SSH_CONNECTION"), "checkenv ignores prefixed variable names");
	unsetenv("SSH_CONNECTION_EXTRA");

	char **saved_environ = environ;
	environ              = nullptr;
	ok &= expect(!checkenv("HOWDY_TEST_PRESENT"), "checkenv handles null environ");
	environ = saved_environ;

	reset_identify_state();
	ok &= expect(pam_sm_authenticate(nullptr, 0, 0, nullptr) == PAM_SUCCESS,
	             "authenticate forwards PAM_SUCCESS from identify");
	ok &= expect(identify_calls == 1, "authenticate calls identify once");
	ok &= expect(last_ask_auth_tok, "authenticate requests auth token");

	reset_identify_state();
	identify_result = PAM_AUTH_ERR;
	ok &= expect(pam_sm_authenticate(nullptr, 0, 0, nullptr) == PAM_AUTH_ERR,
	             "authenticate forwards PAM_AUTH_ERR from identify");
	ok &= expect(identify_calls == 1, "PAM_AUTH_ERR path calls identify once");

	reset_identify_state();
	identify_result = PAM_USER_UNKNOWN;
	ok &= expect(pam_sm_authenticate(nullptr, 0, 0, nullptr) == PAM_USER_UNKNOWN,
	             "authenticate forwards PAM_USER_UNKNOWN from identify");
	ok &= expect(identify_calls == 1, "PAM_USER_UNKNOWN path calls identify once");

	reset_identify_state();
	identify_throw_mode        = 1;
	bool std_exception_escaped = false;
	int  std_exception_result  = PAM_SUCCESS;
	try {
		std_exception_result = pam_sm_authenticate(nullptr, 0, 0, nullptr);
	} catch (...) {
		std_exception_escaped = true;
	}
	ok &= expect(std_exception_result == PAM_SYSTEM_ERR,
	             "authenticate returns PAM_SYSTEM_ERR on std exception");
	ok &= expect(identify_calls == 1, "std exception path calls identify once");
	ok &= expect(!std_exception_escaped, "std exception does not escape authenticate");

	reset_identify_state();
	identify_throw_mode            = 2;
	bool unknown_exception_escaped = false;
	int  unknown_exception_result  = PAM_SUCCESS;
	try {
		unknown_exception_result = pam_sm_authenticate(nullptr, 0, 0, nullptr);
	} catch (...) {
		unknown_exception_escaped = true;
	}
	ok &= expect(unknown_exception_result == PAM_SYSTEM_ERR,
	             "authenticate returns PAM_SYSTEM_ERR on unknown exception");
	ok &= expect(identify_calls == 1, "unknown exception path calls identify once");
	ok &= expect(!unknown_exception_escaped, "unknown exception does not escape authenticate");

	reset_identify_state();
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
