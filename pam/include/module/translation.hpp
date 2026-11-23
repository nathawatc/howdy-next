#pragma once

#include <string>
#include <string_view>

#include <libintl.h>

namespace howdy::pam {
	inline constexpr const char *kGettextDomain = "howdy";

	[[nodiscard]] inline auto Translate(const char *message) -> const char * {
		return dgettext(kGettextDomain, message);
	}

	[[nodiscard]] inline auto ReplaceFirstPlaceholder(std::string      template_text,
	                                                  std::string_view replacement) -> std::string {
		const auto placeholder_pos = template_text.find("{}");
		if (placeholder_pos != std::string::npos) {
			template_text.replace(placeholder_pos, 2, replacement);
		}
		return template_text;
	}

}  // namespace howdy::pam
