#include "module/production_entrypoint.hpp"

#include "module/auth_eligibility.hpp"
#include "module/auth_flow.hpp"
#include "prompt/native_prompt_conversation.hpp"
#include "prompt/observed_prompt_conversation.hpp"
#include "prompt/prompt_coordinator.hpp"
#include "prompt/prompt_submitter.hpp"
#include "runtime/compare_process.hpp"
#include "runtime/runtime_session.hpp"

#include <cerrno>
#include <cstring>
#include <memory>
#include <syslog.h>
#include <tuple>
#include <unistd.h>

#include <security/pam_ext.h>

namespace {

	auto InputPromptPreflight(void *context) -> bool {
		(void)context;
		if (euidaccess("/dev/uinput", W_OK | R_OK) != 0) {
			const int access_errno = errno;
			syslog(LOG_ERR, "Input prompt workaround unavailable: %s (%d)", strerror(access_errno),
			       access_errno);
			return false;
		}
		return true;
	}

	auto RequestAuthToken(void *context, pam_handle_t *pamh) -> std::tuple<int, const char *> {
		(void)context;
		const char *auth_tok_ptr = nullptr;
		const int   auth_result  = pam_get_authtok(pamh, PAM_AUTHTOK, &auth_tok_ptr, nullptr);
		return {auth_result, auth_tok_ptr};
	}

	auto CreatePromptSubmitter(void *context) -> std::unique_ptr<howdy::pam::PromptSubmitter> {
		(void)context;
		return howdy::pam::CreateUinputPromptSubmitter();
	}

	auto CreateNativePrompt(void *context, pam_handle_t *pamh) -> std::unique_ptr<NativePrompt> {
		(void)context;
		return std::make_unique<NativePromptConversation>(pamh);
	}

	auto CreateSecretPromptConversation(void *context, pam_handle_t *pamh,
	                                    howdy::pam::SecretPromptObserver observer)
	    -> std::unique_ptr<howdy::pam::SecretPromptConversation> {
		(void)context;
		return std::make_unique<howdy::pam::ObservedPromptConversation>(pamh, observer);
	}

	void CancelAndReapCompareProcess(void *context, pid_t child_pid) noexcept {
		(void)context;
		howdy::pam::compare_process::CancelAndReap(child_pid);
	}

	auto ProductionPromptCoordinatorDependencies() -> howdy::pam::PromptCoordinatorDependencies {
		return {
		    .spawn_compare_process             = howdy::pam::compare_process::Spawn,
		    .wait_for_compare_process          = howdy::pam::compare_process::Wait,
		    .cancel_and_reap_compare_process   = CancelAndReapCompareProcess,
		    .input_prompt_preflight            = InputPromptPreflight,
		    .create_prompt_submitter           = CreatePromptSubmitter,
		    .create_native_prompt              = CreateNativePrompt,
		    .create_secret_prompt_conversation = CreateSecretPromptConversation,
		    .request_auth_token                = RequestAuthToken,
		};
	}

	auto ProductionIdentifyDependencies() -> howdy::pam::auth_flow::IdentifyDependencies {
		return {
		    .runtime_session    = howdy::pam::ProductionRuntimeSessionDependencies(),
		    .prompt_coordinator = ProductionPromptCoordinatorDependencies(),
		    .eligibility =
		        howdy::pam::auth_eligibility::ProductionAuthenticationEligibilityDependencies(),
		};
	}

	auto ProductionAuthenticate(void *context, pam_handle_t *pamh,
	                            howdy::pam::PamModuleArguments arguments, bool request_auth_token)
	    -> int {
		(void)context;
		return howdy::pam::auth_flow::IdentifyWithDependencies(pamh, arguments, request_auth_token,
		                                                       ProductionIdentifyDependencies());
	}

}  // namespace

namespace howdy::pam {

	auto ProductionEntrypointDependencies() noexcept -> EntrypointDependencies {
		return {
		    .context      = nullptr,
		    .authenticate = ProductionAuthenticate,
		};
	}

}  // namespace howdy::pam
