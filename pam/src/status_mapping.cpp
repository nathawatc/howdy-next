#include "status_mapping.hpp"

#include <libintl.h>
#include <string>
#include <sys/wait.h>

#include <security/pam_modules.h>

#include "main.hpp"

auto map_compare_wait_status(int status) -> CompareStatusDecision {
  CompareStatusDecision decision;
  decision.pam_result = PAM_AUTH_ERR;

  if (WIFEXITED(status)) {
    const int exit_status = WEXITSTATUS(status);
    switch (exit_status) {
    case CompareError::NO_FACE_MODEL:
      decision.log_message = "Failure, no face model known";
      break;
    case CompareError::TIMEOUT_REACHED:
      decision.conversation_kind = ConversationKind::Error;
      decision.conversation_message = gettext("Failure, timeout reached");
      decision.log_message = "Failure, timeout reached";
      break;
    case CompareError::ABORT:
      decision.log_message = "Failure, general abort";
      break;
    case CompareError::TOO_DARK:
      decision.conversation_kind = ConversationKind::Error;
      decision.conversation_message = gettext("Face detection image too dark");
      decision.log_message = "Failure, image too dark";
      break;
    case CompareError::INVALID_DEVICE:
      decision.log_message = "Failure, not possible to open camera at configured path";
      break;
    case CompareError::RUBBERSTAMP:
      decision.log_message = "Failure, rubberstamp mode rejected";
      break;
    default:
      decision.conversation_kind = ConversationKind::Error;
      decision.conversation_message = build_unknown_error_message(exit_status);
      decision.log_message = "Failure, unknown error";
      break;
    }
    return decision;
  }

  if (WIFSIGNALED(status)) {
    decision.log_message =
        "Child killed by signal " + std::string(strsignal(WTERMSIG(status)));
  }

  return decision;
}

auto build_confirmation_message(std::string_view username) -> std::string {
  std::string template_text = gettext("Identified face as {}");
  const auto placeholder_pos = template_text.find("{}");
  if (placeholder_pos != std::string::npos) {
    template_text.replace(placeholder_pos, 2, std::string(username));
  }
  return template_text;
}

auto build_unknown_error_message(int exit_status) -> std::string {
  std::string template_text = gettext("Unknown error: {}");
  const auto placeholder_pos = template_text.find("{}");
  if (placeholder_pos != std::string::npos) {
    template_text.replace(placeholder_pos, 2, std::to_string(exit_status));
  }
  return template_text;
}
