#include "module/confirmation_message.hpp"

#include "module/translation.hpp"

#include <string>
#include <string_view>

auto BuildConfirmationMessage(std::string_view username) -> std::string {
	std::string template_text   = howdy::pam::Translate("Face matched user {}");
	const auto  placeholder_pos = template_text.find("{}");
	if (placeholder_pos != std::string::npos) {
		template_text.replace(placeholder_pos, 2, std::string(username));
	}
	return template_text;
}
