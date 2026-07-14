#include "status_mapping.hpp"

#include "common/compare_exit.hpp"
#include "translation.hpp"

#include <cstring>
#include <string>

#include <security/pam_modules.h>

#include <sys/wait.h>

auto map_compare_wait_status(int status) -> CompareStatusDecision {
	CompareStatusDecision decision;
	decision.pam_result = PAM_AUTH_ERR;

	if (WIFEXITED(status)) {
		const int exit_status = WEXITSTATUS(status);
		switch (static_cast<howdy::native::CompareExit>(exit_status)) {
			case howdy::native::CompareExit::kSuccess:
				decision.pam_result  = PAM_SUCCESS;
				decision.log_message = "Login approved";
				break;
			case howdy::native::CompareExit::kNoFaceModel:
				decision.log_message = "Failure, no face model known";
				break;
			case howdy::native::CompareExit::kTimeoutReached:
				decision.conversation_kind    = ConversationKind::Error;
				decision.conversation_message = howdy::pam::translate("Failure, timeout reached");
				decision.log_message          = "Failure, timeout reached";
				break;
			case howdy::native::CompareExit::kAbort:
				decision.log_message = "Failure, general abort";
				break;
			case howdy::native::CompareExit::kTooDark:
				decision.conversation_kind = ConversationKind::Error;
				decision.conversation_message =
				    howdy::pam::translate("Face detection image too dark");
				decision.log_message = "Failure, image too dark";
				break;
			case howdy::native::CompareExit::kInvalidDevice:
				decision.log_message = "Failure, not possible to open camera at configured path";
				break;
			default:
				decision.conversation_kind    = ConversationKind::Error;
				decision.conversation_message = build_unknown_error_message(exit_status);
				decision.log_message          = "Failure, unknown error";
				break;
		}
		return decision;
	}

	if (WIFSIGNALED(status)) {
		const int   signal_number = WTERMSIG(status);
		const char *signal_text   = strsignal(signal_number);
		if (signal_text != nullptr) {
			decision.log_message =
			    "Child killed by signal " + std::string(signal_text, std::strlen(signal_text));
		} else {
			decision.log_message = "Child killed by signal " + std::to_string(signal_number);
		}
	}

	return decision;
}

auto build_confirmation_message(std::string_view username) -> std::string {
	std::string template_text   = howdy::pam::translate("Identified face as {}");
	const auto  placeholder_pos = template_text.find("{}");
	if (placeholder_pos != std::string::npos) {
		template_text.replace(placeholder_pos, 2, std::string(username));
	}
	return template_text;
}

auto build_unknown_error_message(int exit_status) -> std::string {
	std::string template_text   = howdy::pam::translate("Unknown error: {}");
	const auto  placeholder_pos = template_text.find("{}");
	if (placeholder_pos != std::string::npos) {
		template_text.replace(placeholder_pos, 2, std::to_string(exit_status));
	}
	return template_text;
}
