#ifndef HOWDY_PAM_INCLUDE_MODULE_STATUS_MAPPING_HPP_H
#define HOWDY_PAM_INCLUDE_MODULE_STATUS_MAPPING_HPP_H

#include "module/auth_eligibility.hpp"

#include <cstdint>
#include <string>
#include <string_view>

inline constexpr auto kFaceVerificationSucceededMessage = "Face verification succeeded";

enum class ConversationKind : std::uint8_t {
	kNone,
	kError,
	kInfo
};

struct CompareStatusDecision {
	int              pam_result        = 0;
	ConversationKind conversation_kind = ConversationKind::kNone;
	std::string      conversation_message;
	std::string      log_message;
};

auto MapCompareWaitStatus(int status) -> CompareStatusDecision;
auto BuildConfirmationMessage(std::string_view username) -> std::string;
auto BuildUnknownErrorMessage(int exit_status) -> std::string;

__attribute__((visibility("hidden"))) auto MapAuthenticationEligibility(
    const howdy::pam::auth_eligibility::AuthenticationEligibilityResult &result) -> int;

#endif  // HOWDY_PAM_INCLUDE_MODULE_STATUS_MAPPING_HPP_H
