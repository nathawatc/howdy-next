#include "status_mapping.hh"

#include <csignal>
#include <iostream>
#include <string>

#include <security/pam_modules.h>

namespace {

auto expect(bool condition, const std::string &message) -> bool {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    return false;
  }
  return true;
}

}  // namespace

auto main() -> int {
  bool ok = true;

  {
    const auto decision = map_compare_wait_status(10 << 8);
    ok &= expect(decision.pam_result == PAM_AUTH_ERR,
                 "no-model returns PAM_AUTH_ERR");
    ok &= expect(decision.conversation_kind == ConversationKind::None,
                 "no-model has no conversation");
    ok &= expect(decision.log_message == "Failure, no face model known",
                 "no-model log message");
  }

  {
    const auto decision = map_compare_wait_status(11 << 8);
    ok &= expect(decision.conversation_kind == ConversationKind::Error,
                 "timeout returns error conversation");
    ok &= expect(decision.conversation_message == "Failure, timeout reached",
                 "timeout message");
  }

  {
    const auto decision = map_compare_wait_status(13 << 8);
    ok &= expect(decision.conversation_kind == ConversationKind::Error,
                 "too-dark returns error conversation");
    ok &= expect(decision.conversation_message == "Face detection image too dark",
                 "too-dark message");
  }

  {
    const auto decision = map_compare_wait_status(99 << 8);
    ok &= expect(decision.conversation_kind == ConversationKind::Error,
                 "unknown exit returns error conversation");
    ok &= expect(decision.conversation_message == "Unknown error: 99",
                 "unknown exit message");
    ok &= expect(decision.log_message == "Failure, unknown error",
                 "unknown exit log");
  }

  {
    const auto decision = map_compare_wait_status(SIGTERM);
    ok &= expect(decision.conversation_kind == ConversationKind::None,
                 "signal exit has no conversation");
    ok &= expect(decision.log_message.find("Child killed by signal") == 0,
                 "signal exit log");
  }

  ok &= expect(build_confirmation_message("alice") == "Identified face as alice",
               "confirmation message is formatted");
  ok &= expect(build_unknown_error_message(42) == "Unknown error: 42",
               "unknown error message is formatted");

  if (!ok) {
    return 1;
  }
  return 0;
}
