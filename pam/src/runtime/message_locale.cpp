#include "runtime/message_locale.hpp"

namespace howdy::pam {

	ScopedMessageLocale::ScopedMessageLocale() noexcept
	    : previous_locale_(uselocale(nullptr)) {
		if (previous_locale_ == nullptr) {
			return;
		}

		locale_t duplicated_locale = duplocale(previous_locale_);
		if (duplicated_locale == nullptr) {
			return;
		}

		active_locale_ = newlocale(LC_MESSAGES_MASK | LC_CTYPE_MASK, "", duplicated_locale);
		if (active_locale_ == nullptr) {
			freelocale(duplicated_locale);
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
