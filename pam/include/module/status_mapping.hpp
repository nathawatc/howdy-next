#pragma once

#include "module/auth_eligibility.hpp"

#include <cstdint>
#include <string>

inline constexpr auto kFaceVerificationSucceededMessage = "Face verification succeeded";

enum class ConversationKind : std::uint8_t {
	kNone,
	kError,
};

struct CompareStatusDecision {
	int              pam_result        = 0;
	ConversationKind conversation_kind = ConversationKind::kNone;
	std::string      conversation_message;
	std::string      log_message;
};

auto MapCompareWaitStatus(int status) -> CompareStatusDecision;
auto BuildUnknownErrorMessage(int exit_status) -> std::string;

__attribute__((visibility("hidden"))) auto MapAuthenticationEligibility(
    const howdy::pam::auth_eligibility::AuthenticationEligibilityResult &result) -> int;
