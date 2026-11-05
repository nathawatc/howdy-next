#pragma once

#include "runtime/environment.hpp"

#include <locale.h>  // IWYU pragma: keep

#include <security/pam_appl.h>

namespace howdy::pam {

	// Selects LC_MESSAGES and LC_CTYPE for the calling thread. A locale named by the
	// PAM environment (LC_ALL, then LC_MESSAGES, then LANG) wins, because Debian and
	// Ubuntu set LANG through pam_env rather than the process environment. Without a
	// usable PAM locale it falls back to the process environment.
	class __attribute__((visibility("hidden"))) ScopedMessageLocale {
	public:
		ScopedMessageLocale() noexcept;
		ScopedMessageLocale(pam_handle_t                                 *pamh,
		                    const runtime::EnvironmentLookupDependencies &environment) noexcept;
		~ScopedMessageLocale() noexcept;

		ScopedMessageLocale(const ScopedMessageLocale &)                     = delete;
		auto operator=(const ScopedMessageLocale &) -> ScopedMessageLocale & = delete;
		ScopedMessageLocale(ScopedMessageLocale &&)                          = delete;
		auto operator=(ScopedMessageLocale &&) -> ScopedMessageLocale &      = delete;

	private:
		locale_t previous_locale_ = nullptr;
		locale_t active_locale_   = nullptr;
	};

}  // namespace howdy::pam
