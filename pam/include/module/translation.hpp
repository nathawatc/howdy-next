#pragma once

#include <libintl.h>

namespace howdy::pam {
	inline constexpr const char *kGettextDomain = "howdy";

	inline auto Translate(const char *message) -> const char * {
		return dgettext(kGettextDomain, message);
	}

}  // namespace howdy::pam
