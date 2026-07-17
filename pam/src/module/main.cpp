#include "module/main.hpp"

// Called by PAM when a user needs to be authenticated, for example by running
// the sudo command.
PAM_EXTERN auto pam_sm_authenticate(pam_handle_t *pamh, int flags, int argc, const char **argv)
    -> int {
	const auto production_identify = [](void *, pam_handle_t *handle, PamModuleArguments arguments,
	                                    bool ask_auth_tok) -> int {
		return identify(handle, arguments, ask_auth_tok);
	};
	return authenticate_with_identify(pamh, {.flags = flags, .argc = argc, .argv = argv}, true,
	                                  nullptr, production_identify);
}

// Called by PAM when a session is started, such as by the su command.
// Linux-PAM module ABI requires pam_handle_t *, int, int, const char **.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
PAM_EXTERN auto pam_sm_open_session(pam_handle_t *pamh, int flags, int argc, const char **argv)
    -> int {
	(void)pamh;
	(void)flags;
	(void)argc;
	(void)argv;
	return PAM_IGNORE;
}

// The functions below are required by PAM, but intentionally remain trivial:
// only pam_sm_authenticate enters the C++ auth flow and needs fail-closed
// exception handling.
// Linux-PAM module ABI requires pam_handle_t *, int, int, const char **.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
PAM_EXTERN auto pam_sm_acct_mgmt(pam_handle_t *pamh, int flags, int argc, const char **argv)
    -> int {
	(void)pamh;
	(void)flags;
	(void)argc;
	(void)argv;
	return PAM_IGNORE;
}

// Linux-PAM module ABI requires pam_handle_t *, int, int, const char **.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
PAM_EXTERN auto pam_sm_close_session(pam_handle_t *pamh, int flags, int argc, const char **argv)
    -> int {
	(void)pamh;
	(void)flags;
	(void)argc;
	(void)argv;
	return PAM_IGNORE;
}

// Linux-PAM module ABI requires pam_handle_t *, int, int, const char **.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
PAM_EXTERN auto pam_sm_chauthtok(pam_handle_t *pamh, int flags, int argc, const char **argv)
    -> int {
	(void)pamh;
	(void)flags;
	(void)argc;
	(void)argv;
	return PAM_IGNORE;
}

// Linux-PAM module ABI requires pam_handle_t *, int, int, const char **.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
PAM_EXTERN auto pam_sm_setcred(pam_handle_t *pamh, int flags, int argc, const char **argv) -> int {
	(void)pamh;
	(void)flags;
	(void)argc;
	(void)argv;
	return PAM_IGNORE;
}
