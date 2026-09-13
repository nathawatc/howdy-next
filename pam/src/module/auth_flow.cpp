#include "module/auth_flow.hpp"

#include "config/runtime_config_loader.hpp"
#include "module/auth_eligibility.hpp"
#include "module/pam_options.hpp"
#include "module/status_mapping.hpp"
#include "module/translation.hpp"
#include "prompt/pam_conversation.hpp"
#include "prompt/prompt_coordinator.hpp"
#include "protocol/compare_exit.hpp"
#include "runtime/compare_launch.hpp"
#include "runtime/runtime_session.hpp"

#include <chrono>
#include <cstdlib>
#include <libintl.h>
#include <paths.hpp>
#include <string>
#include <syslog.h>
#include <unistd.h>

#include <security/pam_appl.h>
#include <security/pam_ext.h>
#include <security/pam_modules.h>

#include <sys/types.h>
#include <sys/wait.h>

namespace {
	// Covers exec, model loading, and camera setup before configured scan timeout begins.
	constexpr auto kCompareStartupGrace = std::chrono::seconds(3);

	using howdy::native::CompareExit;
	using howdy::pam::auth_flow::ConversationFn;

	auto CompareStatusIs(int status, CompareExit exit_code) -> bool {
		return WIFEXITED(status) && WEXITSTATUS(status) == static_cast<int>(exit_code);
	}

	auto GetUsername(pam_handle_t *pamh, const char **username) -> int {
		const int result = pam_get_user(pamh, username, nullptr);
		if (result != PAM_SUCCESS || *username == nullptr || (*username)[0] == '\0') {
			syslog(LOG_ERR, "Unable to determine the user.");
			return result == PAM_SUCCESS ? PAM_USER_UNKNOWN : result;
		}
		return PAM_SUCCESS;
	}

	void SendDetectionNotice(const howdy::native::RuntimeConfig &config,
	                         const ConversationFn               &conv_function) {
		// Custom install prefixes require Howdy's domain to map to its configured locale directory.
		bindtextdomain(GETTEXT_PACKAGE, LOCALEDIR);
		if (!config.core.detection_notice) {
			return;
		}
		const int result = conv_function({
		    .style = PAM_TEXT_INFO,
		    .text  = howdy::pam::Translate("Starting face verification"),
		});
		if (result != PAM_SUCCESS) {
			syslog(LOG_ERR, "Unable to show the detection notice.");
		}
	}

	auto MapPromptResult(const howdy::pam::PromptCoordinatorResult &result, const char *username,
	                     const howdy::native::RuntimeConfig &config,
	                     const ConversationFn               &conv_function) -> int {
		switch (result.decision) {
			case howdy::pam::PromptCoordinatorDecision::kPamResult:
				return result.pam_status != PAM_SUCCESS ? result.pam_status : PAM_IGNORE;
			case howdy::pam::PromptCoordinatorDecision::kPasswordFallback:
				return result.pam_status != PAM_SUCCESS
				           ? howdy::pam::auth_flow::HowdyStatus(username, result.compare_status,
				                                                config, conv_function)
				           : PAM_IGNORE;
			case howdy::pam::PromptCoordinatorDecision::kHowdyResult:
				return howdy::pam::auth_flow::HowdyStatus(username, result.compare_status, config,
				                                          conv_function);
			case howdy::pam::PromptCoordinatorDecision::kInvalidDependencies:
			case howdy::pam::PromptCoordinatorDecision::kCompareSpawnFailed:
			case howdy::pam::PromptCoordinatorDecision::kAlreadyRun:
				return PAM_SYSTEM_ERR;
		}
		return PAM_SYSTEM_ERR;
	}

	auto DependenciesValid(const howdy::pam::auth_flow::IdentifyDependencies &dependencies)
	    -> bool {
		const auto &runtime     = dependencies.runtime_session;
		const auto &prompt      = dependencies.prompt_coordinator;
		const auto &eligibility = dependencies.eligibility;
		return runtime.prepare_runtime != nullptr && runtime.load_runtime_config != nullptr &&
		       runtime.effective_uid != nullptr && prompt.spawn_compare_process != nullptr &&
		       prompt.wait_for_compare_process != nullptr &&
		       prompt.cancel_and_reap_compare_process != nullptr &&
		       prompt.input_prompt_preflight != nullptr &&
		       prompt.create_prompt_submitter != nullptr &&
		       prompt.create_native_prompt != nullptr &&
		       prompt.create_secret_prompt_conversation != nullptr &&
		       prompt.request_auth_token != nullptr && eligibility.ssh_session_present != nullptr &&
		       eligibility.read_lid_state != nullptr &&
		       eligibility.check_model_readiness != nullptr;
	}

}  // namespace

namespace howdy::pam::auth_flow {

	auto SendConversationMessage(const ConversationFn &conv_function, int msg_type,
	                             const std::string &message) -> void {
		const int result = conv_function({.style = msg_type, .text = message});
		if (result != PAM_SUCCESS) {
			syslog(LOG_WARNING, "Could not send PAM status message: %d", result);
		}
	}

	auto AuthTokenPresent(pam_handle_t *pamh) -> bool {
		const void *auth_token = nullptr;
		const int   result     = pam_get_item(pamh, PAM_AUTHTOK, &auth_token);
		return result == PAM_SUCCESS && auth_token != nullptr;
	}

	auto HowdyError(int status, const ConversationFn &conv_function) -> int {
		const auto decision = MapCompareWaitStatus(status);
		if (decision.conversation_kind == ConversationKind::kError) {
			SendConversationMessage(conv_function, PAM_ERROR_MSG, decision.conversation_message);
		} else if (decision.conversation_kind == ConversationKind::kInfo) {
			SendConversationMessage(conv_function, PAM_TEXT_INFO, decision.conversation_message);
		}

		if (CompareStatusIs(status, CompareExit::kNoFaceModel)) {
			syslog(LOG_NOTICE, "%s", decision.log_message.c_str());
		} else if (WIFEXITED(status)) {
			syslog(LOG_ERR, "%s", decision.log_message.c_str());
		} else if (WIFSIGNALED(status)) {
			syslog(LOG_ERR, "%s (%d)", decision.log_message.c_str(), WTERMSIG(status));
		}

		return PAM_AUTH_ERR;
	}

	auto HowdyStatus(const char *username, int status, const howdy::native::RuntimeConfig &config,
	                 const ConversationFn &conv_function) -> int {
		if (status != EXIT_SUCCESS) {
			return HowdyError(status, conv_function);
		}

		if (!config.core.no_confirmation) {
			SendConversationMessage(conv_function, PAM_TEXT_INFO,
			                        BuildConfirmationMessage(username));
		}

		syslog(LOG_INFO, "%s", kFaceVerificationSucceededMessage);
		return PAM_SUCCESS;
	}

	namespace {

		void LogEligibilityResult(
		    const howdy::pam::auth_eligibility::AuthenticationEligibilityResult &result) {
			using howdy::pam::auth_eligibility::AuthenticationEligibility;

			if (!result.diagnostic_message.empty()) {
				syslog(LOG_ERR, "%s", result.diagnostic_message.c_str());
			}

			switch (result.status) {
				case AuthenticationEligibility::kEligible:
					return;
				case AuthenticationEligibility::kDisabled:
					syslog(LOG_INFO, "Face verification skipped: Howdy is disabled");
					return;
				case AuthenticationEligibility::kSshSession:
					syslog(LOG_INFO, "Face verification skipped for SSH session");
					return;
				case AuthenticationEligibility::kClosedLid:
					syslog(LOG_INFO, "Face verification skipped: lid is closed");
					return;
				case AuthenticationEligibility::kInvalidUser:
					syslog(LOG_WARNING, "Face verification skipped: invalid username");
					return;
				case AuthenticationEligibility::kMissingModel:
					syslog(LOG_WARNING, "Face verification skipped: no enrolled face model");
					return;
				case AuthenticationEligibility::kInvalidModelStorage:
					if (!result.error_message.empty()) {
						syslog(LOG_ERR, "%s", result.error_message.c_str());
					}
					return;
				case AuthenticationEligibility::kRuntimeError:
					if (result.error_message.empty()) {
						syslog(LOG_ERR, "Authentication eligibility failed");
					} else {
						syslog(LOG_ERR, "%s", result.error_message.c_str());
					}
					return;
			}
		}

	}  // namespace

	auto IdentifyWithDependencies(pam_handle_t *pamh, PamModuleArguments arguments,
	                              bool ask_auth_tok, const IdentifyDependencies &dependencies)
	    -> int {
		(void)arguments.flags;

		openlog("pam_howdy", 0, LOG_AUTHPRIV);

		if (!DependenciesValid(dependencies)) {
			return PAM_SYSTEM_ERR;
		}

		const char *username = nullptr;
		int         pam_res  = GetUsername(pamh, &username);
		if (pam_res != PAM_SUCCESS) {
			return pam_res;
		}

		howdy::pam::RuntimeSession runtime_session(kConfiguredConfigPath, kConfiguredUserModelsDir,
		                                           dependencies.runtime_session);

		const auto runtime_result = runtime_session.LoadForUser(username);
		if (runtime_result.status == howdy::pam::RuntimeSessionLoadStatus::kPrepareFailed ||
		    runtime_result.status == howdy::pam::RuntimeSessionLoadStatus::kInvalidDependencies ||
		    runtime_result.status == howdy::pam::RuntimeSessionLoadStatus::kAlreadyLoaded) {
			return PAM_SYSTEM_ERR;
		}

		if (runtime_result.status != howdy::pam::RuntimeSessionLoadStatus::kOk ||
		    runtime_result.config_result.status != howdy::native::RuntimeConfigLoadStatus::kOk ||
		    !runtime_result.config_result.config.has_value()) {
			syslog(LOG_ERR, "%s", runtime_result.config_result.error_message.c_str());
			return PAM_SYSTEM_ERR;
		}
		const auto &config = *runtime_result.config_result.config;

		const auto eligibility_result =
		    howdy::pam::auth_eligibility::EvaluateAuthenticationEligibility(
		        pamh, config, username, runtime_session.UserModelsDir(), dependencies.eligibility);
		LogEligibilityResult(eligibility_result);
		pam_res = MapAuthenticationEligibility(eligibility_result);
		if (pam_res != PAM_SUCCESS) {
			return pam_res;
		}

		howdy::pam::PamConversation conversation;
		pam_res = howdy::pam::PamConversation::Acquire(pamh, &conversation);
		if (pam_res != PAM_SUCCESS) {
			return pam_res;
		}
		const ConversationFn conv_function =
		    [&conversation](const howdy::pam::ConversationMessage &message) noexcept -> int {
			return conversation.Send(message);
		};

		SendDetectionNotice(config, conv_function);

		const PamOptions pam_options         = ParsePamOptions(arguments);
		const bool       existing_auth_token = auth_flow::AuthTokenPresent(pamh);

		howdy::pam::PromptCoordinator coordinator(
		    pamh, pam_options.workaround, ask_auth_tok, existing_auth_token,
		    dependencies.prompt_coordinator,
		    std::chrono::seconds(config.video.timeout) + kCompareStartupGrace);

		if (!coordinator.Valid()) {
			return PAM_SYSTEM_ERR;
		}

		const howdy::pam::CompareLaunchRequest compare_request = {
		    .config_path     = runtime_session.ConfigPath(),
		    .username        = username,
		    .user_models_dir = runtime_session.UserModelsDir(),
		    .staged_runtime  = runtime_session.Staged(),
		};

		return MapPromptResult(coordinator.Run(compare_request), username, config, conv_function);
	}

}  // namespace howdy::pam::auth_flow
