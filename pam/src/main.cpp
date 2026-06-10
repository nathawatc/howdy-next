#include "main.hpp"

#include <exception>
#include <syslog.h>

namespace {

	auto fail_closed_from_exception(const char *context, const std::exception &error) -> int {
		syslog(LOG_ERR, "Unhandled C++ exception in %s: %s", context, error.what());
		return PAM_AUTH_ERR;
	}

	auto fail_closed_from_unknown_exception(const char *context) -> int {
		syslog(LOG_ERR, "Unhandled non-standard exception in %s", context);
		return PAM_AUTH_ERR;
	}

}  // namespace

// Called by PAM when a user needs to be authenticated, for example by running
// the sudo command.
PAM_EXTERN auto pam_sm_authenticate(pam_handle_t *pamh, int flags, int argc, const char **argv)
    -> int {
	try {
		return identify(pamh, flags, argc, argv, true);
	} catch (const std::exception &error) {
		return fail_closed_from_exception("pam_sm_authenticate", error);
	} catch (...) {
		return fail_closed_from_unknown_exception("pam_sm_authenticate");
	}
}

// Called by PAM when a session is started, such as by the su command.
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
PAM_EXTERN auto pam_sm_acct_mgmt(pam_handle_t *pamh, int flags, int argc, const char **argv)
    -> int {
	(void)pamh;
	(void)flags;
	(void)argc;
	(void)argv;
	return PAM_IGNORE;
}

PAM_EXTERN auto pam_sm_close_session(pam_handle_t *pamh, int flags, int argc, const char **argv)
    -> int {
	(void)pamh;
	(void)flags;
	(void)argc;
	(void)argv;
	return PAM_IGNORE;
}

PAM_EXTERN auto pam_sm_chauthtok(pam_handle_t *pamh, int flags, int argc, const char **argv)
    -> int {
	(void)pamh;
	(void)flags;
	(void)argc;
	(void)argv;
	return PAM_IGNORE;
}

PAM_EXTERN auto pam_sm_setcred(pam_handle_t *pamh, int flags, int argc, const char **argv) -> int {
	(void)pamh;
	(void)flags;
	(void)argc;
	(void)argv;
	return PAM_IGNORE;
}
