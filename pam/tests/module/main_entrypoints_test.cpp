#include "module/main.hpp"
#include "module/translation.hpp"
#include "test_support.hpp"

#include <array>
#include <clocale>
#include <cstdlib>
#include <iostream>
#include <langinfo.h>
#include <libintl.h>
#include <stdexcept>
#include <string>

#include <security/pam_modules.h>

#ifndef HOWDY_TEST_LOCALEDIR
#	define HOWDY_TEST_LOCALEDIR LOCALEDIR
#endif

namespace {

	using howdy::test::expect;

	int         identify_calls      = 0;
	bool        last_ask_auth_tok   = false;
	int         identify_result     = PAM_SUCCESS;
	int         identify_throw_mode = 0;
	locale_t    identify_locale     = nullptr;
	std::string identify_codeset;
	std::string identify_ctype_locale;
	std::string identify_messages_locale;
	std::string identify_numeric_locale;
	std::string identify_time_locale;
	std::string identify_monetary_locale;
	std::string identify_collate_locale;
	std::string identify_message;

	template <std::size_t Size>
	auto first_available_locale(const std::array<const char *, Size> &candidates, int mask) -> const
	    char * {
		for (const char *candidate : candidates) {
			locale_t locale = newlocale(mask, candidate, nullptr);
			if (locale != nullptr) {
				freelocale(locale);
				return candidate;
			}
		}
		return nullptr;
	}

	auto locale_category_name(int category) -> std::string {
		return nl_langinfo(_NL_LOCALE_NAME(category));
	}

	auto reset_identify_state() -> void {
		identify_calls      = 0;
		last_ask_auth_tok   = false;
		identify_result     = PAM_SUCCESS;
		identify_throw_mode = 0;
		identify_locale     = nullptr;
		identify_codeset.clear();
		identify_ctype_locale.clear();
		identify_messages_locale.clear();
		identify_numeric_locale.clear();
		identify_time_locale.clear();
		identify_monetary_locale.clear();
		identify_collate_locale.clear();
		identify_message.clear();
	}

}  // namespace

auto identify(pam_handle_t * /*pamh*/, PamModuleArguments /*arguments*/, bool ask_auth_tok) -> int {
	++identify_calls;
	last_ask_auth_tok        = ask_auth_tok;
	identify_locale          = uselocale(nullptr);
	identify_codeset         = nl_langinfo(CODESET);
	identify_ctype_locale    = locale_category_name(LC_CTYPE);
	identify_messages_locale = locale_category_name(LC_MESSAGES);
	identify_numeric_locale  = locale_category_name(LC_NUMERIC);
	identify_time_locale     = locale_category_name(LC_TIME);
	identify_monetary_locale = locale_category_name(LC_MONETARY);
	identify_collate_locale  = locale_category_name(LC_COLLATE);
	identify_message         = howdy::pam::translate("Attempting facial authentication");
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

	reset_identify_state();
	ok &= expect(pam_sm_authenticate(nullptr, 0, 0, nullptr) == PAM_SUCCESS,
	             "authenticate forwards PAM_SUCCESS from identify");
	ok &= expect(identify_calls == 1, "authenticate calls identify once");
	ok &= expect(last_ask_auth_tok, "authenticate requests auth token");

	const char       *initial_global_locale_ptr = std::setlocale(LC_ALL, nullptr);
	const std::string initial_global_locale =
	    initial_global_locale_ptr == nullptr ? "" : initial_global_locale_ptr;
	const char       *initial_lc_all_ptr   = std::getenv("LC_ALL");
	const bool        had_initial_lc_all   = initial_lc_all_ptr != nullptr;
	const std::string initial_lc_all       = had_initial_lc_all ? initial_lc_all_ptr : "";
	const char       *initial_language_ptr = std::getenv("LANGUAGE");
	const bool        had_initial_language = initial_language_ptr != nullptr;
	const std::string initial_language     = had_initial_language ? initial_language_ptr : "";
	const char       *initial_domain_ptr   = textdomain(nullptr);
	const std::string initial_domain      = initial_domain_ptr == nullptr ? "" : initial_domain_ptr;
	const char       *initial_binding_ptr = bindtextdomain(GETTEXT_PACKAGE, nullptr);
	const std::string initial_binding = initial_binding_ptr == nullptr ? "" : initial_binding_ptr;

	std::setlocale(LC_ALL, "C");
	locale_t previous_locale        = newlocale(LC_ALL_MASK, "C", nullptr);
	locale_t original_thread_locale = uselocale(nullptr);
	if (previous_locale != nullptr) {
		original_thread_locale = uselocale(previous_locale);
	}
	const locale_t    active_previous_locale = uselocale(nullptr);
	const std::string previous_codeset       = nl_langinfo(CODESET);
	textdomain("pam-host-test-domain");
	const std::string host_domain = textdomain(nullptr);
	bindtextdomain(GETTEXT_PACKAGE, HOWDY_TEST_LOCALEDIR);
	setenv("LANGUAGE", "th", 1);

	const std::array<const char *, 1> thai_locales = {"th_TH.UTF-8"};
	const char                       *thai_locale =
	    first_available_locale(thai_locales, LC_MESSAGES_MASK | LC_CTYPE_MASK);
	if (thai_locale != nullptr) {
		setenv("LC_ALL", "th_TH.UTF-8", 1);
		reset_identify_state();
		ok &= expect(pam_sm_authenticate(nullptr, 0, 0, nullptr) == PAM_SUCCESS,
		             "authenticate succeeds with environment-selected message locale");
		ok &= expect(identify_message == "กำลังยืนยันตัวตนด้วยใบหน้า",
		             "authenticate translates exact environment-selected message");
		ok &= expect(identify_codeset == "UTF-8",
		             "authenticate uses UTF-8 LC_CTYPE while message locale is active");
		ok &= expect(identify_ctype_locale == "th_TH.UTF-8",
		             "authenticate selects environment LC_CTYPE");
		ok &= expect(identify_messages_locale == "th_TH.UTF-8",
		             "authenticate selects environment LC_MESSAGES");
		ok &= expect(identify_numeric_locale == "C", "authenticate preserves LC_NUMERIC");
		ok &= expect(identify_time_locale == "C", "authenticate preserves LC_TIME");
		ok &= expect(identify_monetary_locale == "C", "authenticate preserves LC_MONETARY");
		ok &= expect(identify_collate_locale == "C", "authenticate preserves LC_COLLATE");
		ok &= expect(identify_locale != active_previous_locale,
		             "authenticate activates environment-selected thread locale");
		ok &= expect(std::string(std::setlocale(LC_ALL, nullptr)) == "C",
		             "authenticate leaves global locale as C");
		ok &= expect(uselocale(nullptr) == active_previous_locale,
		             "authenticate restores previous thread locale");
		ok &= expect(std::string(nl_langinfo(CODESET)) == previous_codeset,
		             "authenticate restores previous thread codeset");
		ok &= expect(std::string(textdomain(nullptr)) == host_domain,
		             "authenticate preserves host gettext domain");
	} else {
		std::cerr << "SKIP: th_TH.UTF-8 locale unavailable; PAM locale assertions not run\n";
	}

	setenv("LC_ALL", "howdy-invalid-locale", 1);
	reset_identify_state();
	ok &= expect(pam_sm_authenticate(nullptr, 0, 0, nullptr) == PAM_SUCCESS,
	             "locale creation failure does not fail authentication");
	ok &= expect(identify_locale == active_previous_locale,
	             "locale creation failure keeps existing thread locale");
	ok &= expect(uselocale(nullptr) == active_previous_locale,
	             "locale creation failure preserves previous thread locale");
	ok &= expect(std::string(std::setlocale(LC_ALL, nullptr)) == "C",
	             "locale creation failure leaves global locale as C");

	if (previous_locale != nullptr) {
		uselocale(original_thread_locale);
		freelocale(previous_locale);
	}
	if (had_initial_lc_all) {
		setenv("LC_ALL", initial_lc_all.c_str(), 1);
	} else {
		unsetenv("LC_ALL");
	}
	if (had_initial_language) {
		setenv("LANGUAGE", initial_language.c_str(), 1);
	} else {
		unsetenv("LANGUAGE");
	}
	if (!initial_binding.empty()) {
		bindtextdomain(GETTEXT_PACKAGE, initial_binding.c_str());
	}
	if (!initial_domain.empty()) {
		textdomain(initial_domain.c_str());
	}
	if (!initial_global_locale.empty()) {
		std::setlocale(LC_ALL, initial_global_locale.c_str());
	}

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
	identify_throw_mode                  = 1;
	const locale_t std_exception_locale  = uselocale(nullptr);
	bool           std_exception_escaped = false;
	int            std_exception_result  = PAM_SUCCESS;
	try {
		std_exception_result = pam_sm_authenticate(nullptr, 0, 0, nullptr);
	} catch (...) {
		std_exception_escaped = true;
	}
	ok &= expect(std_exception_result == PAM_SYSTEM_ERR,
	             "authenticate returns PAM_SYSTEM_ERR on std exception");
	ok &= expect(identify_calls == 1, "std exception path calls identify once");
	ok &= expect(!std_exception_escaped, "std exception does not escape authenticate");
	ok &= expect(uselocale(nullptr) == std_exception_locale,
	             "std exception path restores previous thread locale");

	reset_identify_state();
	identify_throw_mode                      = 2;
	const locale_t unknown_exception_locale  = uselocale(nullptr);
	bool           unknown_exception_escaped = false;
	int            unknown_exception_result  = PAM_SUCCESS;
	try {
		unknown_exception_result = pam_sm_authenticate(nullptr, 0, 0, nullptr);
	} catch (...) {
		unknown_exception_escaped = true;
	}
	ok &= expect(unknown_exception_result == PAM_SYSTEM_ERR,
	             "authenticate returns PAM_SYSTEM_ERR on unknown exception");
	ok &= expect(identify_calls == 1, "unknown exception path calls identify once");
	ok &= expect(!unknown_exception_escaped, "unknown exception does not escape authenticate");
	ok &= expect(uselocale(nullptr) == unknown_exception_locale,
	             "unknown exception path restores previous thread locale");

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
