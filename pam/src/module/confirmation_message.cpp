#include "module/confirmation_message.hpp"

#include "module/translation.hpp"

#include <string>
#include <string_view>

auto BuildConfirmationMessage(std::string_view username) -> std::string {
	return howdy::pam::ReplaceFirstPlaceholder(howdy::pam::Translate("Face matched user {}"),
	                                           username);
}
