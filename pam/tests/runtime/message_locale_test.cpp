#include "pam_test_support.hpp"
#include "runtime/environment.hpp"
#include "runtime/message_locale.hpp"
#include "test_support.hpp"

#include <array>
#include <clocale>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

#include <langinfo.h>
#include <libintl.h>
#include <locale.h>
#include <stdlib.h>

namespace {

	using howdy::pam::ScopedMessageLocale;
	using howdy::test::Expect;
	using ScopedEnvironment = howdy::test::ScopedEnvironmentVariable;

	auto CurrentGlobalLocale() -> std::string {
		const char *locale = std::setlocale(LC_ALL, nullptr);
		return locale == nullptr ? std::string{} : std::string(locale);
	}

	auto CurrentTextDomain() -> std::string {
		const char *domain = textdomain(nullptr);
		return domain == nullptr ? std::string{} : std::string(domain);
	}

	template <std::size_t size>
	auto FirstAvailableLocale(const std::array<const char *, size> &candidates, int mask) -> const
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

	auto LocaleCategoryName(locale_t locale, int category) -> std::string {
		return nl_langinfo_l(_NL_LOCALE_NAME(category), locale);
	}

	auto ExpectEnvironmentSelectedCategories() -> bool {
		const std::array<const char *, 2> candidates = {"C.UTF-8", "C.utf8"};
		const char *const                 selected_locale =
		    FirstAvailableLocale(candidates, LC_MESSAGES_MASK | LC_CTYPE_MASK);
		if (selected_locale == nullptr) {
			return howdy::test::SkipUnavailable("locale-c-utf8", "C.UTF-8 locale is not available");
		}

		locale_t expected_locale =
		    newlocale(LC_MESSAGES_MASK | LC_CTYPE_MASK, selected_locale, nullptr);
		locale_t baseline_locale = newlocale(LC_ALL_MASK, "C", nullptr);
		if (expected_locale == nullptr || baseline_locale == nullptr) {
			if (expected_locale != nullptr) {
				freelocale(expected_locale);
			}
			if (baseline_locale != nullptr) {
				freelocale(baseline_locale);
			}
			return true;
		}

		const locale_t    previous_locale = uselocale(baseline_locale);
		ScopedEnvironment environment("LC_ALL");
		environment.Set(selected_locale);
		bool ok = true;
		{
			ScopedMessageLocale message_locale;
			const locale_t      active_locale = uselocale(nullptr);
			ok &= Expect(LocaleCategoryName(active_locale, LC_MESSAGES) ==
			                 LocaleCategoryName(expected_locale, LC_MESSAGES),
			             "locale guard applies environment-selected LC_MESSAGES");
			ok &= Expect(LocaleCategoryName(active_locale, LC_CTYPE) ==
			                 LocaleCategoryName(expected_locale, LC_CTYPE),
			             "locale guard applies environment-selected LC_CTYPE");
		}
		ok &= Expect(uselocale(nullptr) == baseline_locale,
		             "locale guard restores locale after category activation");
		(void)uselocale(previous_locale);
		freelocale(expected_locale);
		freelocale(baseline_locale);
		return ok;
	}

	// A PAM environment with the three locale variables; nullptr means unset.
	struct FakePamEnvironment {
		const char *lc_all      = nullptr;
		const char *lc_messages = nullptr;
		const char *lang        = nullptr;
		int         lookups     = 0;
	};

	auto FakePamLookup(void *context, pam_handle_t *pamh, const char *name) -> const char * {
		auto *environment = static_cast<FakePamEnvironment *>(context);
		if (pamh != howdy::test::OpaquePamHandleForTest()) {
			return nullptr;
		}
		++environment->lookups;
		const std::string_view variable(name);
		if (variable == "LC_ALL") {
			return environment->lc_all;
		}
		if (variable == "LC_MESSAGES") {
			return environment->lc_messages;
		}
		return variable == "LANG" ? environment->lang : nullptr;
	}

	// The names of the LC_MESSAGES and LC_CTYPE categories while a guard built from
	// `pam_environment` and the given process-environment LC_ALL is active. The thread
	// starts in the "C" locale so any change is visible.
	auto ActiveLocaleNames(FakePamEnvironment &pam_environment, const char *process_lc_all)
	    -> std::string {
		locale_t baseline_locale = newlocale(LC_ALL_MASK, "C", nullptr);
		if (baseline_locale == nullptr) {
			return "baseline-unavailable";
		}
		const locale_t previous_locale = uselocale(baseline_locale);

		ScopedEnvironment       lc_all("LC_ALL");
		const ScopedEnvironment lc_messages("LC_MESSAGES");
		const ScopedEnvironment lc_ctype("LC_CTYPE");
		const ScopedEnvironment lang("LANG");
		if (process_lc_all != nullptr) {
			lc_all.Set(process_lc_all);
		}

		const howdy::pam::runtime::EnvironmentLookupDependencies dependencies{
		    .context             = &pam_environment,
		    .pam_environment     = FakePamLookup,
		    .process_environment = nullptr,
		};
		std::string names;
		{
			ScopedMessageLocale message_locale(howdy::test::OpaquePamHandleForTest(), dependencies);
			const locale_t      active_locale = uselocale(nullptr);
			names = LocaleCategoryName(active_locale, LC_MESSAGES) + "|" +
			        LocaleCategoryName(active_locale, LC_CTYPE);
		}
		const bool restored = uselocale(nullptr) == baseline_locale;
		(void)uselocale(previous_locale);
		freelocale(baseline_locale);
		return restored ? names : names + "|not-restored";
	}

	auto ExpectPamEnvironmentSelection() -> bool {
		const std::array<const char *, 2> candidates = {"C.UTF-8", "C.utf8"};
		const char *const utf8 = FirstAvailableLocale(candidates, LC_MESSAGES_MASK | LC_CTYPE_MASK);
		if (utf8 == nullptr) {
			return howdy::test::SkipUnavailable("locale-c-utf8", "C.UTF-8 locale is not available");
		}
		locale_t          utf8_locale = newlocale(LC_MESSAGES_MASK | LC_CTYPE_MASK, utf8, nullptr);
		const std::string utf8_name   = LocaleCategoryName(utf8_locale, LC_MESSAGES);
		freelocale(utf8_locale);
		const std::string utf8_names = utf8_name + "|" + utf8_name;
		const std::string c_names    = "C|C";

		bool ok = true;
		{
			FakePamEnvironment pam{.lang = utf8};
			ok &= Expect(ActiveLocaleNames(pam, "C") == utf8_names,
			             "PAM LANG beats a process LC_ALL");
		}
		{
			FakePamEnvironment pam{.lc_all = "C", .lang = utf8};
			ok &= Expect(ActiveLocaleNames(pam, utf8) == c_names, "PAM LC_ALL beats PAM LANG");
		}
		{
			FakePamEnvironment pam{.lc_all = utf8, .lang = "C"};
			ok &= Expect(ActiveLocaleNames(pam, nullptr) == utf8_names,
			             "PAM LC_ALL selects the locale");
		}
		{
			FakePamEnvironment pam{.lc_messages = utf8, .lang = "C"};
			ok &= Expect(ActiveLocaleNames(pam, nullptr) == utf8_names,
			             "PAM LC_MESSAGES beats PAM LANG");
		}
		{
			FakePamEnvironment pam{.lc_all = "", .lc_messages = "", .lang = utf8};
			ok &= Expect(ActiveLocaleNames(pam, nullptr) == utf8_names,
			             "empty PAM values are skipped");
		}
		{
			FakePamEnvironment pam{};
			ok &= Expect(ActiveLocaleNames(pam, utf8) == utf8_names,
			             "empty PAM environment falls back to the process environment");
			ok &= Expect(pam.lookups == 3, "all three PAM locale variables are consulted");
		}
		{
			FakePamEnvironment pam{.lang = "xx_XX.UTF-8"};
			ok &= Expect(ActiveLocaleNames(pam, utf8) == utf8_names,
			             "uninstalled PAM locale falls back to the process environment");
		}

		const std::string                long_name(200, 'a');
		const std::array<std::string, 7> hostile = {
		    "../x",       "/tmp/x",  ".hidden", std::string("C.UTF-8\n"),
		    "th_TH/../x", long_name, "C UTF-8",
		};
		for (const std::string &name : hostile) {
			FakePamEnvironment pam{.lang = name.c_str()};
			ok &= Expect(ActiveLocaleNames(pam, utf8) == utf8_names,
			             "hostile PAM locale name is rejected, process environment is used");
		}
		{
			// An invalid higher-precedence name is not skipped in favor of a valid LANG.
			FakePamEnvironment pam{.lc_all = "../x", .lang = utf8};
			ok &= Expect(ActiveLocaleNames(pam, nullptr) == c_names,
			             "invalid PAM LC_ALL does not fall through to LANG");
		}
		return ok;
	}

	auto ExpectThaiFromPamEnvironment() -> bool {
		const std::array<const char *, 2> candidates = {"th_TH.UTF-8", "th_TH.utf8"};
		const char *const thai = FirstAvailableLocale(candidates, LC_MESSAGES_MASK | LC_CTYPE_MASK);
		if (thai == nullptr) {
			return howdy::test::SkipUnavailable("locale-th", "th_TH.UTF-8 locale is not available");
		}
		locale_t          thai_locale = newlocale(LC_MESSAGES_MASK | LC_CTYPE_MASK, thai, nullptr);
		const std::string thai_name   = LocaleCategoryName(thai_locale, LC_MESSAGES);
		freelocale(thai_locale);

		FakePamEnvironment pam{.lang = thai};
		return Expect(ActiveLocaleNames(pam, "C") == thai_name + "|" + thai_name,
		              "PAM LANG selects Thai while the process environment is C");
	}

}  // namespace

static_assert(!std::is_copy_constructible_v<ScopedMessageLocale>);
static_assert(!std::is_copy_assignable_v<ScopedMessageLocale>);

auto main() -> int {
	bool ok = true;

	const locale_t    host_locale   = uselocale(nullptr);
	const std::string global_locale = CurrentGlobalLocale();
	const std::string text_domain   = CurrentTextDomain();
	{
		ScopedMessageLocale message_locale;
	}
	ok &= Expect(uselocale(nullptr) == host_locale, "normal scope restores host thread locale");
	ok &= Expect(CurrentGlobalLocale() == global_locale,
	             "normal scope leaves process-global locale unchanged");
	ok &= Expect(CurrentTextDomain() == text_domain,
	             "normal scope leaves host gettext domain unchanged");
	ok &= ExpectEnvironmentSelectedCategories();
	ok &= ExpectPamEnvironmentSelection();
	ok &= ExpectThaiFromPamEnvironment();

	bool threw = false;
	try {
		ScopedMessageLocale message_locale;
		throw std::runtime_error("message locale test");
	} catch (const std::runtime_error &) {
		threw = true;
	}
	ok &= Expect(threw, "message locale scope permits exception propagation");
	ok &= Expect(uselocale(nullptr) == host_locale,
	             "scope restores host thread locale after exception");

	{
		ScopedEnvironment invalid_locale("LC_ALL");
		invalid_locale.Set("howdy-invalid-locale");
		ScopedMessageLocale message_locale;
		ok &= Expect(uselocale(nullptr) == host_locale,
		             "failed locale setup leaves existing thread locale usable");
	}
	ok &= Expect(uselocale(nullptr) == host_locale,
	             "failed locale setup remains safe during destruction");
	ok &= Expect(CurrentGlobalLocale() == global_locale,
	             "failed locale setup leaves process-global locale unchanged");

	return ok ? 0 : 1;
}
