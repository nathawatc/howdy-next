#include "status_mapping.hh"

#include <string>
#include <sys/wait.h>

#include <security/pam_modules.h>

#include "main.hh"

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
      decision.conversation_message = "Failure, timeout reached";
      decision.log_message = "Failure, timeout reached";
      break;
    case CompareError::ABORT:
      decision.log_message = "Failure, general abort";
      break;
    case CompareError::TOO_DARK:
      decision.conversation_kind = ConversationKind::Error;
      decision.conversation_message = "Face detection image too dark";
      decision.log_message = "Failure, image too dark";
      break;
    case CompareError::INVALID_DEVICE:
      decision.log_message = "Failure, not possible to open camera at configured path";
      break;
    default:
      decision.conversation_kind = ConversationKind::Error;
      decision.conversation_message = "Unknown error: " + std::to_string(exit_status);
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
  return "Identified face as " + std::string(username);
}
