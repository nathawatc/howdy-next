#pragma once

#include "module/auth_eligibility.hpp"
#include "module/pam_options.hpp"
#include "prompt/pam_conversation.hpp"
#include "prompt/prompt_coordinator.hpp"
#include "runtime/runtime_session.hpp"

#include <functional>
#include <string>

#include <security/pam_appl.h>

namespace howdy::pam::auth_flow {

	using ConversationFn = std::function<int(const howdy::pam::ConversationMessage &)>;

	struct IdentifyDependencies {
		RuntimeSessionDependencies                                          runtime_session;
		PromptCoordinatorDependencies                                       prompt_coordinator;
		howdy::pam::auth_eligibility::AuthenticationEligibilityDependencies eligibility;
	};

	__attribute__((visibility("hidden"))) auto ProductionIdentifyDependencies()
	    -> IdentifyDependencies;

	__attribute__((visibility("hidden"))) auto
	IdentifyWithDependencies(pam_handle_t *pamh, PamModuleArguments arguments, bool ask_auth_tok,
	                         const IdentifyDependencies &dependencies) -> int;

	__attribute__((visibility("hidden"))) void
	SendConversationMessage(const ConversationFn &conv_function, int msg_type,
	                        const std::string &message);

	__attribute__((visibility("hidden"))) auto AuthTokenPresent(pam_handle_t *pamh) -> bool;

	__attribute__((visibility("hidden"))) auto HowdyError(int                   status,
	                                                      const ConversationFn &conv_function)
	    -> int;

	__attribute__((visibility("hidden"))) auto
	HowdyStatus(const char *username, int status, const howdy::native::RuntimeConfig &config,
	            const ConversationFn &conv_function) -> int;

}  // namespace howdy::pam::auth_flow
