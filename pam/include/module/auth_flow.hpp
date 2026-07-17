#pragma once

#include "prompt/prompt_coordinator.hpp"
#include "runtime/runtime_session.hpp"

#include <filesystem>
#include <functional>
#include <string>

#include <security/pam_appl.h>

namespace howdy::pam::auth_flow {

	using ConversationFn = std::function<int(int, const char *)>;

	using CheckEnabledFn = int (*)(void *context, const howdy::native::RuntimeConfig &config,
	                               const char                  *username,
	                               const std::filesystem::path &user_models_dir);

	struct IdentifyDependencies {
		void                         *context = nullptr;
		RuntimeSessionDependencies    runtime_session;
		PromptCoordinatorDependencies prompt_coordinator;
		CheckEnabledFn                check_enabled = nullptr;
	};

	__attribute__((visibility("hidden"))) auto production_identify_dependencies()
	    -> IdentifyDependencies;

	__attribute__((visibility("hidden"))) auto
	identify_with_dependencies(pam_handle_t *pamh, PamModuleArguments arguments, bool ask_auth_tok,
	                           const IdentifyDependencies &dependencies) -> int;

	__attribute__((visibility("hidden"))) void
	send_conversation_message(const ConversationFn &conv_function, int msg_type,
	                          const std::string &message);

	__attribute__((visibility("hidden"))) auto make_conversation(pam_handle_t   *pamh,
	                                                             ConversationFn *conv_function)
	    -> int;

	__attribute__((visibility("hidden"))) auto auth_token_present(pam_handle_t *pamh) -> bool;

	__attribute__((visibility("hidden"))) auto howdy_error(int                   status,
	                                                       const ConversationFn &conv_function)
	    -> int;

	__attribute__((visibility("hidden"))) auto
	howdy_status(const char *username, int status, const howdy::native::RuntimeConfig &config,
	             const ConversationFn &conv_function) -> int;

	__attribute__((visibility("hidden"))) auto
	check_enabled(const howdy::native::RuntimeConfig &config, const char *username,
	              const std::filesystem::path &user_models_dir) -> int;

}  // namespace howdy::pam::auth_flow
