#pragma once

#include <libintl.h>

namespace howdy::pam {

	inline auto Translate(const char *message) -> const char * {
		return dgettext(GETTEXT_PACKAGE, message);
	}

}  // namespace howdy::pam
