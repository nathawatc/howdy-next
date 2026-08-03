#include "runtime/message_locale.hpp"
#include "test_support.hpp"

#include <array>
#include <clocale>
#include <cstdlib>
#include <langinfo.h>
#include <libintl.h>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace {

	using howdy::pam::ScopedMessageLocale;
	using howdy::test::expect;

	class ScopedEnvironment {
	public:
		explicit ScopedEnvironment(const char *name)
		    : name_(name) {
			const char *value = std::getenv(name_.c_str());
			if (value != nullptr) {
				had_original_ = true;
				original_     = value;
			}
		}

		ScopedEnvironment(const ScopedEnvironment &)                     = delete;
		auto operator=(const ScopedEnvironment &) -> ScopedEnvironment & = delete;

		~ScopedEnvironment() {
			if (had_original_) {
				setenv(name_.c_str(), original_.c_str(), 1);
			} else {
				unsetenv(name_.c_str());
			}
		}

		void set(const char *value) {
			setenv(name_.c_str(), value, 1);
		}

	private:
		std::string name_;
		std::string original_;
		bool        had_original_ = false;
	};

	auto current_global_locale() -> std::string {
		const char *locale = std::setlocale(LC_ALL, nullptr);
		return locale == nullptr ? std::string{} : std::string(locale);
	}

	auto current_text_domain() -> std::string {
		const char *domain = textdomain(nullptr);
		return domain == nullptr ? std::string{} : std::string(domain);
	}

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

	auto locale_category_name(locale_t locale, int category) -> std::string {
		return nl_langinfo_l(_NL_LOCALE_NAME(category), locale);
	}

	auto expect_environment_selected_categories() -> bool {
		const std::array<const char *, 2> candidates = {"C.UTF-8", "C.utf8"};
		const char *const                 selected_locale =
		    first_available_locale(candidates, LC_MESSAGES_MASK | LC_CTYPE_MASK);
		if (selected_locale == nullptr) {
			return true;
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
		environment.set(selected_locale);
		bool ok = true;
		{
			ScopedMessageLocale message_locale;
			const locale_t      active_locale = uselocale(nullptr);
			ok &= expect(locale_category_name(active_locale, LC_MESSAGES) ==
			                 locale_category_name(expected_locale, LC_MESSAGES),
			             "locale guard applies environment-selected LC_MESSAGES");
			ok &= expect(locale_category_name(active_locale, LC_CTYPE) ==
			                 locale_category_name(expected_locale, LC_CTYPE),
			             "locale guard applies environment-selected LC_CTYPE");
		}
		ok &= expect(uselocale(nullptr) == baseline_locale,
		             "locale guard restores locale after category activation");
		(void)uselocale(previous_locale);
		freelocale(expected_locale);
		freelocale(baseline_locale);
		return ok;
	}

}  // namespace

static_assert(!std::is_copy_constructible_v<ScopedMessageLocale>);
static_assert(!std::is_copy_assignable_v<ScopedMessageLocale>);

auto main() -> int {
	bool ok = true;

	const locale_t    host_locale   = uselocale(nullptr);
	const std::string global_locale = current_global_locale();
	const std::string text_domain   = current_text_domain();
	{
		ScopedMessageLocale message_locale;
	}
	ok &= expect(uselocale(nullptr) == host_locale, "normal scope restores host thread locale");
	ok &= expect(current_global_locale() == global_locale,
	             "normal scope leaves process-global locale unchanged");
	ok &= expect(current_text_domain() == text_domain,
	             "normal scope leaves host gettext domain unchanged");
	ok &= expect_environment_selected_categories();

	bool threw = false;
	try {
		ScopedMessageLocale message_locale;
		throw std::runtime_error("message locale test");
	} catch (const std::runtime_error &) {
		threw = true;
	}
	ok &= expect(threw, "message locale scope permits exception propagation");
	ok &= expect(uselocale(nullptr) == host_locale,
	             "scope restores host thread locale after exception");

	{
		ScopedEnvironment invalid_locale("LC_ALL");
		invalid_locale.set("howdy-invalid-locale");
		ScopedMessageLocale message_locale;
		ok &= expect(uselocale(nullptr) == host_locale,
		             "failed locale setup leaves existing thread locale usable");
	}
	ok &= expect(uselocale(nullptr) == host_locale,
	             "failed locale setup remains safe during destruction");
	ok &= expect(current_global_locale() == global_locale,
	             "failed locale setup leaves process-global locale unchanged");

	return ok ? 0 : 1;
}
