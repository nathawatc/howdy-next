#include "module/status_mapping.hpp"

#include "module/translation.hpp"
#include "protocol/compare_exit.hpp"

#include <cstring>
#include <string>

#include <security/pam_modules.h>

#include <sys/wait.h>

namespace {
	constexpr auto kChildKilledBySignalPrefix = "Child killed by signal ";
}

auto MapAuthenticationEligibility(
    const howdy::pam::auth_eligibility::AuthenticationEligibilityResult &result) -> int {
	using howdy::pam::auth_eligibility::AuthenticationEligibility;
	switch (result.status) {
		case AuthenticationEligibility::kEligible:
			return PAM_SUCCESS;
		case AuthenticationEligibility::kDisabled:
		case AuthenticationEligibility::kSshSession:
		case AuthenticationEligibility::kClosedLid:
		case AuthenticationEligibility::kInvalidUser:
		case AuthenticationEligibility::kMissingModel:
		case AuthenticationEligibility::kInvalidModelStorage:
		case AuthenticationEligibility::kRuntimeError:
			return PAM_AUTHINFO_UNAVAIL;
	}
	return PAM_SYSTEM_ERR;
}

auto MapCompareWaitStatus(int status) -> CompareStatusDecision {
	CompareStatusDecision decision;
	decision.pam_result = PAM_AUTH_ERR;

	if (WIFEXITED(status)) {
		const int exit_status = WEXITSTATUS(status);
		switch (static_cast<howdy::native::CompareExit>(exit_status)) {
			case howdy::native::CompareExit::kSuccess:
				decision.pam_result  = PAM_SUCCESS;
				decision.log_message = kFaceVerificationSucceededMessage;
				break;
			case howdy::native::CompareExit::kNoFaceModel:
				decision.log_message = "Face verification unavailable: no enrolled face model";
				break;
			case howdy::native::CompareExit::kTimeoutReached:
				decision.conversation_kind = ConversationKind::kError;
				decision.conversation_message =
				    howdy::pam::Translate("Face verification timed out");
				decision.log_message = "Face verification timed out";
				break;
			case howdy::native::CompareExit::kAbort:
				decision.log_message = "Face verification aborted";
				break;
			case howdy::native::CompareExit::kTooDark:
				decision.conversation_kind = ConversationKind::kError;
				decision.conversation_message =
				    howdy::pam::Translate("Camera image is too dark for detection");
				decision.log_message = "Face verification failed: camera image too dark";
				break;
			case howdy::native::CompareExit::kInvalidDevice:
				decision.log_message = "Face verification failed: cannot open configured camera";
				break;
			default:
				decision.conversation_kind    = ConversationKind::kError;
				decision.conversation_message = BuildUnknownErrorMessage(exit_status);
				decision.log_message          = "Face verification failed: unknown error";
				break;
		}
		return decision;
	}

	if (WIFSIGNALED(status)) {
		const int   signal_number = WTERMSIG(status);
		const char *signal_text   = strsignal(signal_number);
		if (signal_text != nullptr) {
			decision.log_message = std::string(kChildKilledBySignalPrefix) +
			                       std::string(signal_text, std::strlen(signal_text));
		} else {
			decision.log_message =
			    std::string(kChildKilledBySignalPrefix) + std::to_string(signal_number);
		}
	}

	return decision;
}

auto BuildConfirmationMessage(std::string_view username) -> std::string {
	std::string template_text   = howdy::pam::Translate("Face matched user {}");
	const auto  placeholder_pos = template_text.find("{}");
	if (placeholder_pos != std::string::npos) {
		template_text.replace(placeholder_pos, 2, std::string(username));
	}
	return template_text;
}

auto BuildUnknownErrorMessage(int exit_status) -> std::string {
	std::string template_text   = howdy::pam::Translate("Unknown error: {}");
	const auto  placeholder_pos = template_text.find("{}");
	if (placeholder_pos != std::string::npos) {
		template_text.replace(placeholder_pos, 2, std::to_string(exit_status));
	}
	return template_text;
}
