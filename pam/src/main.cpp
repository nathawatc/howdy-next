#include "main.hpp"

#include <clocale>  // IWYU pragma: keep
#include <exception>
#include <syslog.h>

namespace {

	class ScopedMessageLocale {
	public:
		ScopedMessageLocale() noexcept
		    : previous_locale_(uselocale(nullptr)) {
			if (previous_locale_ == nullptr) {
				return;
			}

			locale_t duplicated_locale = duplocale(previous_locale_);
			if (duplicated_locale == nullptr) {
				return;
			}

			locale_ = newlocale(LC_MESSAGES_MASK | LC_CTYPE_MASK, "", duplicated_locale);
			if (locale_ == nullptr) {
				freelocale(duplicated_locale);
				return;
			}

			if (uselocale(locale_) == nullptr) {
				freelocale(locale_);
				locale_ = nullptr;
			}
		}

		~ScopedMessageLocale() {
			if (locale_ != nullptr) {
				uselocale(previous_locale_);
				freelocale(locale_);
			}
		}

		ScopedMessageLocale(const ScopedMessageLocale &)                     = delete;
		auto operator=(const ScopedMessageLocale &) -> ScopedMessageLocale & = delete;

	private:
		locale_t locale_          = nullptr;
		locale_t previous_locale_ = nullptr;
	};

	auto system_error_from_exception(const char *context, const std::exception &error) -> int {
		syslog(LOG_ERR, "Unhandled C++ exception in %s: %s", context, error.what());
		return PAM_SYSTEM_ERR;
	}

	auto system_error_from_unknown_exception(const char *context) -> int {
		syslog(LOG_ERR, "Unhandled non-standard exception in %s", context);
		return PAM_SYSTEM_ERR;
	}

}  // namespace

// Called by PAM when a user needs to be authenticated, for example by running
// the sudo command.
PAM_EXTERN auto pam_sm_authenticate(pam_handle_t *pamh, int flags, int argc, const char **argv)
    -> int {
	ScopedMessageLocale message_locale;
	try {
		return identify(pamh, {.flags = flags, .argc = argc, .argv = argv}, true);
	} catch (const std::exception &error) {
		return system_error_from_exception("pam_sm_authenticate", error);
	} catch (...) {
		return system_error_from_unknown_exception("pam_sm_authenticate");
	}
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
