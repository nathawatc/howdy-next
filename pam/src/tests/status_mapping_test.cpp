#include "common/compare_exit.hpp"
#include "status_mapping.hpp"

#include <csignal>
#include <iostream>
#include <string>

#include <security/pam_modules.h>

#include <sys/wait.h>

namespace {

	auto expect(bool condition, const std::string &message) -> bool {
		if (!condition) {
			std::cerr << "FAIL: " << message << "\n";
			return false;
		}
		return true;
	}

	auto make_status(howdy::native::CompareExit exit_code) -> int {
		return static_cast<int>(exit_code) << 8;
	}

}  // namespace

auto main() -> int {
	bool ok = true;

	{
		const auto decision =
		    map_compare_wait_status(make_status(howdy::native::CompareExit::kSuccess));
		ok &= expect(decision.pam_result == PAM_SUCCESS, "success returns PAM_SUCCESS");
		ok &= expect(decision.conversation_kind == ConversationKind::None,
		             "success has no conversation");
		ok &= expect(decision.log_message == "Login approved", "success log message");
	}

	{
		const auto decision =
		    map_compare_wait_status(make_status(howdy::native::CompareExit::kNoFaceModel));
		ok &= expect(decision.pam_result == PAM_AUTH_ERR, "no-model returns PAM_AUTH_ERR");
		ok &= expect(decision.conversation_kind == ConversationKind::None,
		             "no-model has no conversation");
		ok &=
		    expect(decision.log_message == "Failure, no face model known", "no-model log message");
	}

	{
		const auto decision =
		    map_compare_wait_status(make_status(howdy::native::CompareExit::kTimeoutReached));
		ok &= expect(decision.conversation_kind == ConversationKind::Error,
		             "timeout returns error conversation");
		ok &=
		    expect(decision.conversation_message == "Failure, timeout reached", "timeout message");
	}

	{
		const auto decision =
		    map_compare_wait_status(make_status(howdy::native::CompareExit::kAbort));
		ok &= expect(decision.conversation_kind == ConversationKind::None,
		             "abort has no conversation");
		ok &= expect(decision.log_message == "Failure, general abort", "abort log message");
	}

	{
		const auto decision =
		    map_compare_wait_status(make_status(howdy::native::CompareExit::kTooDark));
		ok &= expect(decision.conversation_kind == ConversationKind::Error,
		             "too-dark returns error conversation");
		ok &= expect(decision.conversation_message == "Face detection image too dark",
		             "too-dark message");
	}

	{
		const auto decision =
		    map_compare_wait_status(make_status(howdy::native::CompareExit::kInvalidDevice));
		ok &= expect(decision.pam_result == PAM_AUTH_ERR, "invalid-device fails closed");
		ok &= expect(decision.conversation_kind == ConversationKind::None,
		             "invalid-device has no conversation");
		ok &= expect(decision.log_message ==
		                 "Failure, not possible to open camera at configured path",
		             "invalid-device log message");
	}

	{
		const auto decision =
		    map_compare_wait_status(make_status(howdy::native::CompareExit::kRubberstamp));
		ok &= expect(decision.pam_result == PAM_AUTH_ERR, "rubberstamp fails closed");
		ok &= expect(decision.conversation_kind == ConversationKind::None,
		             "rubberstamp has no conversation");
		ok &= expect(decision.log_message == "Failure, rubberstamp mode rejected",
		             "rubberstamp log message");
	}

	{
		const auto decision = map_compare_wait_status(99 << 8);
		ok &= expect(decision.pam_result == PAM_AUTH_ERR, "unknown exit fails closed");
		ok &= expect(decision.conversation_kind == ConversationKind::Error,
		             "unknown exit returns error conversation");
		ok &= expect(decision.conversation_message == "Unknown error: 99", "unknown exit message");
		ok &= expect(decision.log_message == "Failure, unknown error", "unknown exit log");
	}

	{
		const auto decision = map_compare_wait_status(SIGTERM);
		ok &= expect(decision.pam_result == PAM_AUTH_ERR, "signal exit fails closed");
		ok &= expect(decision.conversation_kind == ConversationKind::None,
		             "signal exit has no conversation");
		ok &= expect(decision.log_message.starts_with("Child killed by signal"), "signal exit log");
	}

	{
		const auto decision = map_compare_wait_status(W_STOPCODE(SIGSTOP));
		ok &= expect(decision.pam_result == PAM_AUTH_ERR, "stopped status fails closed");
		ok &= expect(decision.conversation_kind == ConversationKind::None,
		             "stopped status has no conversation");
		ok &= expect(decision.log_message.empty(), "stopped status has no misleading log");
	}

	{
		const auto decision = map_compare_wait_status(127 << 8);
		ok &= expect(decision.pam_result == PAM_AUTH_ERR,
		             "helper execution-style failure fails closed");
		ok &= expect(decision.conversation_kind == ConversationKind::Error,
		             "helper execution-style failure reports controlled error");
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
