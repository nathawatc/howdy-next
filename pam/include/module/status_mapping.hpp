#ifndef STATUS_MAPPING_H_
#define STATUS_MAPPING_H_

#include "module/auth_eligibility.hpp"

#include <cstdint>
#include <string>
#include <string_view>

inline constexpr auto kFaceVerificationSucceededMessage = "Face verification succeeded";

enum class ConversationKind : std::uint8_t {
	None,
	Error,
	Info
};

struct CompareStatusDecision {
	int              pam_result        = 0;
	ConversationKind conversation_kind = ConversationKind::None;
	std::string      conversation_message;
	std::string      log_message;
};

auto map_compare_wait_status(int status) -> CompareStatusDecision;
auto build_confirmation_message(std::string_view username) -> std::string;
auto build_unknown_error_message(int exit_status) -> std::string;

__attribute__((visibility("hidden"))) auto map_authentication_eligibility(
    const howdy::pam::auth_eligibility::AuthenticationEligibilityResult &result) -> int;

#endif  // STATUS_MAPPING_H_
