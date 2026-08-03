#pragma once

#include <clocale>  // IWYU pragma: keep

namespace howdy::pam {

	class __attribute__((visibility("hidden"))) ScopedMessageLocale {
	public:
		ScopedMessageLocale() noexcept;
		~ScopedMessageLocale() noexcept;

		ScopedMessageLocale(const ScopedMessageLocale &)                     = delete;
		auto operator=(const ScopedMessageLocale &) -> ScopedMessageLocale & = delete;

	private:
		locale_t previous_locale_ = nullptr;
		locale_t active_locale_   = nullptr;
	};

}  // namespace howdy::pam
