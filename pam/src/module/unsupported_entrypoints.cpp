#include <security/pam_modules.h>

// Session, account, password, and credential hooks are intentionally ignored.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
PAM_EXTERN auto pam_sm_open_session(pam_handle_t *pamh, int flags, int argc, const char **argv)
    -> int {
	(void)pamh;
	(void)flags;
	(void)argc;
	(void)argv;
	return PAM_IGNORE;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
PAM_EXTERN auto pam_sm_acct_mgmt(pam_handle_t *pamh, int flags, int argc, const char **argv)
    -> int {
	(void)pamh;
	(void)flags;
	(void)argc;
	(void)argv;
	return PAM_IGNORE;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
PAM_EXTERN auto pam_sm_close_session(pam_handle_t *pamh, int flags, int argc, const char **argv)
    -> int {
	(void)pamh;
	(void)flags;
	(void)argc;
	(void)argv;
	return PAM_IGNORE;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
PAM_EXTERN auto pam_sm_chauthtok(pam_handle_t *pamh, int flags, int argc, const char **argv)
    -> int {
	(void)pamh;
	(void)flags;
	(void)argc;
	(void)argv;
	return PAM_IGNORE;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
PAM_EXTERN auto pam_sm_setcred(pam_handle_t *pamh, int flags, int argc, const char **argv) -> int {
	(void)pamh;
	(void)flags;
	(void)argc;
	(void)argv;
	return PAM_IGNORE;
}
