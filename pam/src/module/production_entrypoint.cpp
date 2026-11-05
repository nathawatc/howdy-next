#include "module/production_entrypoint.hpp"

#include "module/auth_eligibility.hpp"
#include "module/auth_flow.hpp"
#include "module/entrypoint.hpp"
#include "module/pam_options.hpp"
#include "prompt/native_prompt_conversation.hpp"
#include "prompt/observed_prompt_conversation.hpp"
#include "prompt/prompt_coordinator.hpp"
#include "prompt/prompt_submitter.hpp"
#include "runtime/compare_launch.hpp"
#include "runtime/compare_process.hpp"
#include "runtime/environment.hpp"
#include "runtime/runtime_session.hpp"
#include "support/error_message.hpp"

#include <cerrno>
#include <memory>
#include <stdexcept>
#include <tuple>

#include <unistd.h>

#include <security/pam_ext.h>

#include <sys/syslog.h>

namespace {

	auto InputPromptPreflight(void *context) -> bool {
		(void)context;
		if (euidaccess("/dev/uinput", W_OK | R_OK) != 0) {
			const int access_errno = errno;
			syslog(LOG_AUTHPRIV | LOG_ERR,
			       "pam_howdy: Input prompt workaround unavailable: %s (%d)",
			       howdy::native::SystemErrorMessage(access_errno).c_str(), access_errno);
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

	auto SpawnCompareProcess(void *context, const howdy::pam::CompareLaunchRequest &request,
	                         pid_t *child_pid) -> int {
		auto const *operations = static_cast<howdy::pam::compare_process::Operations *>(context);
		if (operations == nullptr) {
			return EINVAL;
		}
		return howdy::pam::compare_process::Spawn(request, child_pid, *operations);
	}

	void CancelAndReapCompareProcess(void *context, pid_t child_pid) noexcept {
		(void)context;
		howdy::pam::compare_process::CancelAndReap(child_pid);
	}

	auto ProductionIdentifyDependencies() -> howdy::pam::auth_flow::IdentifyDependencies {
		static auto compare_operations = howdy::pam::compare_process::ProductionOperations();
		return {
		    .runtime_session = howdy::pam::ProductionRuntimeSessionOperations(),
		    .prompt_coordinator =
		        howdy::pam::ProductionPromptCoordinatorOperations(compare_operations),
		    .eligibility =
		        howdy::pam::auth_eligibility::ProductionAuthenticationEligibilityOperations(),
		};
	}

}  // namespace

namespace howdy::pam {

	auto ProductionAuthenticate(void *context, pam_handle_t *pamh, PamModuleArguments arguments,
	                            bool request_auth_token) -> int {
		(void)context;
		return auth_flow::IdentifyWithDependencies(pamh, arguments, request_auth_token,
		                                           ProductionIdentifyDependencies());
	}

	auto ProductionPromptCoordinatorOperations(compare_process::Operations &compare_operations)
	    -> PromptCoordinatorOperations {
		auto operations = PromptCoordinatorOperations::Create(
		    &compare_operations, SpawnCompareProcess, compare_process::Wait,
		    CancelAndReapCompareProcess, InputPromptPreflight, CreatePromptSubmitter,
		    CreateNativePrompt, CreateSecretPromptConversation, RequestAuthToken);
		if (!operations.has_value()) {
			throw std::logic_error("Failed to create production prompt coordinator operations");
		}
		return *operations;
	}

	auto ProductionEntrypointDependencies() noexcept -> EntrypointDependencies {
		return {
		    .context      = nullptr,
		    .authenticate = ProductionAuthenticate,
		    .environment  = runtime::ProductionEnvironmentLookupDependencies(),
		};
	}

}  // namespace howdy::pam
