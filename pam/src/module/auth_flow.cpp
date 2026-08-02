#include "module/auth_flow.hpp"

#include "module/main.hpp"
#include "module/status_mapping.hpp"
#include "module/translation.hpp"
#include "prompt/conversation_response.hpp"
#include "prompt/prompt_coordinator.hpp"
#include "protocol/compare_exit.hpp"
#include "runtime/runtime_session.hpp"
#include "storage/user_model_readiness.hpp"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <glob.h>
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

	auto compare_status_is(int status, CompareExit exit_code) -> bool {
		return WIFEXITED(status) && WEXITSTATUS(status) == static_cast<int>(exit_code);
	}

	auto get_username(pam_handle_t *pamh, const char **username) -> int {
		const int result = pam_get_user(pamh, username, nullptr);
		if (result != PAM_SUCCESS || *username == nullptr || (*username)[0] == '\0') {
			syslog(LOG_ERR, "Failed to get username");
			return result == PAM_SUCCESS ? PAM_USER_UNKNOWN : result;
		}
		return PAM_SUCCESS;
	}

	void send_detection_notice(const howdy::native::RuntimeConfig &config,
	                           const ConversationFn               &conv_function) {
		// Custom install prefixes require Howdy's domain to map to its configured locale directory.
		bindtextdomain(GETTEXT_PACKAGE, LOCALEDIR);
		if (!config.core.detection_notice) {
			return;
		}
		const int result =
		    conv_function(PAM_TEXT_INFO, howdy::pam::translate("Attempting facial authentication"));
		if (result != PAM_SUCCESS) {
			syslog(LOG_ERR, "Failed to send detection notice");
		}
	}

	auto map_prompt_result(const howdy::pam::PromptCoordinatorResult &result, const char *username,
	                       const howdy::native::RuntimeConfig &config,
	                       const ConversationFn               &conv_function) -> int {
		switch (result.decision) {
			case howdy::pam::PromptCoordinatorDecision::kPamResult:
				return result.pam_status != PAM_SUCCESS ? result.pam_status : PAM_IGNORE;
			case howdy::pam::PromptCoordinatorDecision::kPasswordFallback:
				return result.pam_status != PAM_SUCCESS
				           ? howdy::pam::auth_flow::howdy_status(username, result.compare_status,
				                                                 config, conv_function)
				           : PAM_IGNORE;
			case howdy::pam::PromptCoordinatorDecision::kHowdyResult:
				return howdy::pam::auth_flow::howdy_status(username, result.compare_status, config,
				                                           conv_function);
			case howdy::pam::PromptCoordinatorDecision::kInvalidDependencies:
			case howdy::pam::PromptCoordinatorDecision::kCompareSpawnFailed:
			case howdy::pam::PromptCoordinatorDecision::kAlreadyRun:
				return PAM_SYSTEM_ERR;
		}
		return PAM_SYSTEM_ERR;
	}

	auto dependencies_valid(const howdy::pam::auth_flow::IdentifyDependencies &dependencies)
	    -> bool {
		const auto &runtime = dependencies.runtime_session;
		const auto &prompt  = dependencies.prompt_coordinator;
		return runtime.prepare_runtime != nullptr && runtime.cleanup_runtime != nullptr &&
		       runtime.load_runtime_config != nullptr && runtime.effective_uid != nullptr &&
		       prompt.spawn_compare_process != nullptr &&
		       prompt.wait_for_compare_process != nullptr &&
		       prompt.input_prompt_preflight != nullptr && prompt.create_enter_device != nullptr &&
		       prompt.create_native_prompt != nullptr &&
		       prompt.create_secret_prompt_conversation != nullptr &&
		       prompt.request_auth_token != nullptr && dependencies.check_enabled != nullptr;
	}

	auto production_check_enabled(void *context, const howdy::native::RuntimeConfig &config,
	                              const char                  *username,
	                              const std::filesystem::path &user_models_dir) -> int;

}  // namespace

namespace howdy::pam::auth_flow {

	auto send_conversation_message(const ConversationFn &conv_function, int msg_type,
	                               const std::string &message) -> void {
		const int result = conv_function(msg_type, message.c_str());
		if (result != PAM_SUCCESS) {
			syslog(LOG_WARNING, "Failed to send PAM conversation message: %d", result);
		}
	}

	auto make_conversation(pam_handle_t *pamh, ConversationFn *conv_function) -> int {
		struct pam_conv *conv     = nullptr;
		const void     **conv_ptr = const_cast<const void **>(reinterpret_cast<void **>(&conv));
		const int        pam_res  = pam_get_item(pamh, PAM_CONV, conv_ptr);
		if (pam_res != PAM_SUCCESS) {
			syslog(LOG_ERR, "Failed to acquire conversation");
			return pam_res;
		}

		if (conv == nullptr || conv->conv == nullptr) {
			syslog(LOG_ERR, "PAM conversation is not available");
			return PAM_SYSTEM_ERR;
		}

		const struct pam_conv original_conv = *conv;
		*conv_function = [original_conv](int msg_type, const char *msg_str) -> int {
			const struct pam_message  msg  = {.msg_style = msg_type, .msg = msg_str};
			const struct pam_message *msgp = &msg;
			struct pam_response      *resp = nullptr;
			const int conv_result = original_conv.conv(1, &msgp, &resp, original_conv.appdata_ptr);
			howdy::pam::secure_free_conversation_responses(&resp, 1);
			return conv_result;
		};

		return PAM_SUCCESS;
	}

	auto auth_token_present(pam_handle_t *pamh) -> bool {
		const void *auth_token = nullptr;
		const int   result     = pam_get_item(pamh, PAM_AUTHTOK, &auth_token);
		return result == PAM_SUCCESS && auth_token_item_present(auth_token);
	}

	auto howdy_error(int status, const ConversationFn &conv_function) -> int {
		const auto decision = map_compare_wait_status(status);
		if (decision.conversation_kind == ConversationKind::Error) {
			send_conversation_message(conv_function, PAM_ERROR_MSG, decision.conversation_message);
		} else if (decision.conversation_kind == ConversationKind::Info) {
			send_conversation_message(conv_function, PAM_TEXT_INFO, decision.conversation_message);
		}

		if (compare_status_is(status, CompareExit::kNoFaceModel)) {
			syslog(LOG_NOTICE, "%s", decision.log_message.c_str());
		} else if (WIFEXITED(status)) {
			syslog(LOG_ERR, "%s", decision.log_message.c_str());
		} else if (WIFSIGNALED(status)) {
			syslog(LOG_ERR, "%s (%d)", decision.log_message.c_str(), WTERMSIG(status));
		}

		return PAM_AUTH_ERR;
	}

	auto howdy_status(const char *username, int status, const howdy::native::RuntimeConfig &config,
	                  const ConversationFn &conv_function) -> int {
		if (status != EXIT_SUCCESS) {
			return howdy_error(status, conv_function);
		}

		if (!config.core.no_confirmation) {
			send_conversation_message(conv_function, PAM_TEXT_INFO,
			                          build_confirmation_message(username));
		}

		syslog(LOG_INFO, "Login approved");
		return PAM_SUCCESS;
	}

	auto check_enabled(const howdy::native::RuntimeConfig &config, const char *username,
	                   const std::filesystem::path &user_models_dir) -> int {
		if (config.core.disabled) {
			syslog(LOG_INFO, "Skipped authentication, Howdy is disabled");
			return PAM_AUTHINFO_UNAVAIL;
		}

		if (config.core.abort_if_ssh) {
			if (checkenv("SSH_CONNECTION") || checkenv("SSH_CLIENT") || checkenv("SSH_TTY") ||
			    checkenv("SSHD_OPTS")) {
				syslog(LOG_INFO, "Skipped authentication, SSH session detected");
				return PAM_AUTHINFO_UNAVAIL;
			}
		}

		if (config.core.abort_if_lid_closed) {
			glob_t    glob_result{};
			const int return_value =
			    glob("/proc/acpi/button/lid/*/state", 0, nullptr, &glob_result);

			if (return_value != 0 && return_value != GLOB_NOMATCH) {
				syslog(LOG_ERR, "Failed to read files from glob: %d", return_value);
				if (errno != 0) {
					syslog(LOG_ERR, "Underlying error: %s (%d)", strerror(errno), errno);
				}
			} else {
				for (size_t i = 0; i < glob_result.gl_pathc; i++) {
					std::ifstream file(std::string(glob_result.gl_pathv[i]));
					std::string   lid_state;
					std::getline(file, lid_state);

					if (lid_state.contains("closed")) {
						globfree(&glob_result);
						syslog(LOG_INFO, "Skipped authentication, closed lid detected");
						return PAM_AUTHINFO_UNAVAIL;
					}
				}
			}
			globfree(&glob_result);
		}

		const auto readiness = howdy::native::check_user_model_readiness(user_models_dir, username,
		                                                                 static_cast<uid_t>(0));
		switch (readiness.status) {
			case howdy::native::UserModelStatus::kOk:
				break;
			case howdy::native::UserModelStatus::kInvalidUser:
				syslog(LOG_WARNING, "Skipped authentication, invalid username");
				return PAM_AUTHINFO_UNAVAIL;
			case howdy::native::UserModelStatus::kNoModel:
			case howdy::native::UserModelStatus::kNoModelDirectory:
				syslog(LOG_WARNING, "Skipped authentication, no face model found for user");
				return PAM_AUTHINFO_UNAVAIL;
			default:
				if (!readiness.error_message.empty()) {
					syslog(LOG_ERR, "%s", readiness.error_message.c_str());
				}
				return PAM_AUTHINFO_UNAVAIL;
		}

		return PAM_SUCCESS;
	}

	auto production_identify_dependencies() -> IdentifyDependencies {
		return {
		    .context            = nullptr,
		    .runtime_session    = production_runtime_session_dependencies(),
		    .prompt_coordinator = production_prompt_coordinator_dependencies(),
		    .check_enabled      = production_check_enabled,
		};
	}

	auto identify_with_dependencies(pam_handle_t *pamh, PamModuleArguments arguments,
	                                bool ask_auth_tok, const IdentifyDependencies &dependencies)
	    -> int {
		(void)arguments.flags;

		openlog("pam_howdy", 0, LOG_AUTHPRIV);

		if (!dependencies_valid(dependencies)) {
			return PAM_SYSTEM_ERR;
		}

		const char *username = nullptr;
		int         pam_res  = get_username(pamh, &username);
		if (pam_res != PAM_SUCCESS) {
			return pam_res;
		}

		howdy::pam::RuntimeSession runtime_session(kConfiguredConfigPath, kConfiguredUserModelsDir,
		                                           dependencies.runtime_session);

		const auto runtime_result = runtime_session.load_for_user(username);
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

		pam_res = dependencies.check_enabled(dependencies.context, config, username,
		                                     runtime_session.user_models_dir());
		if (pam_res != PAM_SUCCESS) {
			return pam_res;
		}

		ConversationFn conv_function;
		pam_res = make_conversation(pamh, &conv_function);
		if (pam_res != PAM_SUCCESS) {
			return pam_res;
		}

		send_detection_notice(config, conv_function);

		const Workaround workaround          = get_pam_workaround(arguments.argc, arguments.argv);
		const bool       existing_auth_token = auth_flow::auth_token_present(pamh);

		howdy::pam::PromptCoordinator coordinator(
		    pamh, workaround, ask_auth_tok, existing_auth_token, dependencies.prompt_coordinator,
		    std::chrono::seconds(config.video.timeout) + kCompareStartupGrace);

		if (!coordinator.valid()) {
			return PAM_SYSTEM_ERR;
		}

		const howdy::pam::CompareLaunchRequest compare_request = {
		    .config_path     = runtime_session.config_path(),
		    .username        = username,
		    .user_models_dir = runtime_session.user_models_dir(),
		    .staged_runtime  = runtime_session.staged(),
		};

		const auto report_input_failure = [&conv_function] -> void {
			send_conversation_message(
			    conv_function, PAM_ERROR_MSG,
			    howdy::pam::translate(
			        "Failed to send Enter press, waiting for user to press it instead"));
		};
		return map_prompt_result(coordinator.run(compare_request, report_input_failure), username,
		                         config, conv_function);
	}

}  // namespace howdy::pam::auth_flow

namespace {

	auto production_check_enabled(void *context, const howdy::native::RuntimeConfig &config,
	                              const char                  *username,
	                              const std::filesystem::path &user_models_dir) -> int {
		(void)context;
		return howdy::pam::auth_flow::check_enabled(config, username, user_models_dir);
	}

}  // namespace

auto identify(pam_handle_t *pamh, PamModuleArguments arguments, bool ask_auth_tok) -> int {
	return howdy::pam::auth_flow::identify_with_dependencies(
	    pamh, arguments, ask_auth_tok, howdy::pam::auth_flow::production_identify_dependencies());
}
