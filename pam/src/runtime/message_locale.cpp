#include "runtime/message_locale.hpp"

#include "runtime/environment.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>

#include <locale.h>

namespace howdy::pam {

	namespace {

		constexpr std::size_t kMaxLocaleNameLength = 64;

		// The PAM environment can be set by the user being authenticated, so only
		// plain locale names (language_TERRITORY.codeset@modifier) are accepted.
		auto ValidLocaleName(std::string_view name) -> bool {
			if (name.empty() || name.size() > kMaxLocaleNameLength || name.front() == '.') {
				return false;
			}
			return std::ranges::all_of(name, [](char character) -> bool {
				const bool alphanumeric = (character >= 'a' && character <= 'z') ||
				                          (character >= 'A' && character <= 'Z') ||
				                          (character >= '0' && character <= '9');
				return alphanumeric || character == '_' || character == '.' || character == '@' ||
				       character == '-';
			});
		}

		// Copies the first non-empty LC_ALL, LC_MESSAGES, or LANG from the PAM
		// environment, the glibc precedence. Returns false when none is set or the
		// chosen one is not a valid name; the caller then uses the process environment.
		auto FindPamLocaleName(pam_handle_t                                 *pamh,
		                       const runtime::EnvironmentLookupDependencies &environment,
		                       std::array<char, kMaxLocaleNameLength + 1>   &buffer) noexcept
		    -> bool {
			if (environment.pam_environment == nullptr) {
				return false;
			}
			for (const char *variable : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
				const char *value =
				    environment.pam_environment(environment.context, pamh, variable);
				if (value == nullptr || *value == '\0') {
					continue;
				}
				const std::string_view name(value);
				if (!ValidLocaleName(name)) {
					return false;
				}
				*std::ranges::copy(name, buffer.begin()).out = '\0';
				return true;
			}
			return false;
		}

		// Returns a copy of the thread's locale with LC_MESSAGES and LC_CTYPE taken
		// from `name`, or nullptr. An empty name reads the process environment.
		auto BuildMessageLocale(locale_t base, const char *name) noexcept -> locale_t {
			locale_t duplicated_locale = duplocale(base);
			if (duplicated_locale == nullptr) {
				return nullptr;
			}
			locale_t selected =
			    newlocale(LC_MESSAGES_MASK | LC_CTYPE_MASK, name, duplicated_locale);
			if (selected == nullptr) {
				freelocale(duplicated_locale);
			}
			return selected;
		}

	}  // namespace

	ScopedMessageLocale::ScopedMessageLocale() noexcept
	    : ScopedMessageLocale(nullptr, runtime::EnvironmentLookupDependencies{}) {}

	ScopedMessageLocale::ScopedMessageLocale(
	    pam_handle_t *pamh, const runtime::EnvironmentLookupDependencies &environment) noexcept
	    : previous_locale_(uselocale(nullptr)) {
		if (previous_locale_ == nullptr) {
			return;
		}

		std::array<char, kMaxLocaleNameLength + 1> pam_locale_name{};
		if (FindPamLocaleName(pamh, environment, pam_locale_name)) {
			active_locale_ = BuildMessageLocale(previous_locale_, pam_locale_name.data());
		}
		if (active_locale_ == nullptr) {
			active_locale_ = BuildMessageLocale(previous_locale_, "");
		}
		if (active_locale_ == nullptr) {
			return;
		}

		if (uselocale(active_locale_) == nullptr) {
			freelocale(active_locale_);
			active_locale_ = nullptr;
		}
	}

	ScopedMessageLocale::~ScopedMessageLocale() noexcept {
		if (active_locale_ == nullptr) {
			return;
		}
		(void)uselocale(previous_locale_);
		freelocale(active_locale_);
	}

}  // namespace howdy::pam
