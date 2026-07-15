#include "common/compare_exit.hpp"
#include "storage/user_model_readiness.hpp"
#ifdef HOWDY_PAM_TESTING
#	include "auth_flow_testing.hpp"
#endif
#include "main.hpp"
#include "prompt_coordinator.hpp"
#include "runtime_session.hpp"
#include "status_mapping.hpp"
#include "translation.hpp"

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

#ifdef HOWDY_PAM_TESTING
	howdy::pam::testing::IdentifyDependencies g_identify_dependencies;
	bool                                      g_identify_dependencies_set = false;
#endif

	using howdy::native::CompareExit;

	auto compare_status_is(int status, CompareExit exit_code) -> bool {
		return WIFEXITED(status) && WEXITSTATUS(status) == static_cast<int>(exit_code);
	}

	using ConversationFn = std::function<int(int, const char *)>;

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
			if (resp != nullptr) {
				if (resp->resp != nullptr) {
					std::memset(resp->resp, 0, std::strlen(resp->resp));
					std::free(resp->resp);
				}
				std::free(resp);
			}
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

	auto get_username(pam_handle_t *pamh, char **username) -> int {
		const int result = pam_get_user(pamh, const_cast<const char **>(username), nullptr);
		if (result != PAM_SUCCESS || *username == nullptr || (*username)[0] == '\0') {
			syslog(LOG_ERR, "Failed to get username");
			return result == PAM_SUCCESS ? PAM_USER_UNKNOWN : result;
		}
		return PAM_SUCCESS;
	}

	auto runtime_session_dependencies() -> howdy::pam::RuntimeSessionDependencies {
		auto dependencies = howdy::pam::production_runtime_session_dependencies();
#ifdef HOWDY_PAM_TESTING
		if (g_identify_dependencies_set) {
			dependencies = g_identify_dependencies.runtime_session;
		}
#endif
		return dependencies;
	}

	auto run_enabled_check(const howdy::native::RuntimeConfig &config, const char *username,
	                       const std::filesystem::path &user_models_dir) -> int {
#ifdef HOWDY_PAM_TESTING
		if (g_identify_dependencies_set && g_identify_dependencies.check_enabled != nullptr) {
			return g_identify_dependencies.check_enabled(g_identify_dependencies.context, config,
			                                             username, user_models_dir);
		}
#endif
		return check_enabled(config, username, user_models_dir);
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

	auto prompt_coordinator_dependencies() -> howdy::pam::PromptCoordinatorDependencies {
		auto dependencies = howdy::pam::production_prompt_coordinator_dependencies();
#ifdef HOWDY_PAM_TESTING
		if (g_identify_dependencies_set) {
			dependencies = g_identify_dependencies.prompt_coordinator;
		}
#endif
		return dependencies;
	}

	auto map_prompt_result(const howdy::pam::PromptCoordinatorResult &result, const char *username,
	                       const howdy::native::RuntimeConfig &config,
	                       const ConversationFn               &conv_function) -> int {
		switch (result.decision) {
			case howdy::pam::PromptCoordinatorDecision::kPamResult:
				return result.pam_status != PAM_SUCCESS ? result.pam_status : PAM_IGNORE;
			case howdy::pam::PromptCoordinatorDecision::kPasswordFallback:
				return result.pam_status != PAM_SUCCESS
				           ? howdy_status(username, result.compare_status, config, conv_function)
				           : PAM_IGNORE;
			case howdy::pam::PromptCoordinatorDecision::kHowdyResult:
				if (result.enter_failed) {
					send_conversation_message(
					    conv_function, PAM_ERROR_MSG,
					    howdy::pam::translate(
					        "Failed to send Enter press, waiting for user to press it instead"));
				}
				return howdy_status(username, result.compare_status, config, conv_function);
			case howdy::pam::PromptCoordinatorDecision::kInvalidDependencies:
			case howdy::pam::PromptCoordinatorDecision::kCompareSpawnFailed:
			case howdy::pam::PromptCoordinatorDecision::kAlreadyRun:
				return PAM_SYSTEM_ERR;
		}
		return PAM_SYSTEM_ERR;
	}

}  // namespace

#ifdef HOWDY_PAM_TESTING
namespace howdy::pam::testing {

	auto set_identify_dependencies(const IdentifyDependencies &dependencies) -> void {
		g_identify_dependencies     = dependencies;
		g_identify_dependencies_set = true;
	}

	auto reset_identify_dependencies() -> void {
		g_identify_dependencies     = {};
		g_identify_dependencies_set = false;
	}

	auto send_conversation_message(const ConversationFn &conv_function, int msg_type,
	                               const std::string &message) -> void {
		::send_conversation_message(conv_function, msg_type, message);
	}

	auto make_conversation(pam_handle_t *pamh, ConversationFn *conv_function) -> int {
		return ::make_conversation(pamh, conv_function);
	}

	auto auth_token_present(pam_handle_t *pamh) -> bool {
		return ::auth_token_present(pamh);
	}

	auto howdy_error(int status, const ConversationFn &conv_function) -> int {
		return ::howdy_error(status, conv_function);
	}

	auto howdy_status(const char *username, int status, const howdy::native::RuntimeConfig &config,
	                  const ConversationFn &conv_function) -> int {
		return ::howdy_status(username, status, config, conv_function);
	}

	auto check_enabled(const howdy::native::RuntimeConfig &config, const char *username,
	                   const std::filesystem::path &user_models_dir) -> int {
		return ::check_enabled(config, username, user_models_dir);
	}

}  // namespace howdy::pam::testing
#endif

auto identify(pam_handle_t *pamh, PamModuleArguments arguments, bool ask_auth_tok) -> int {
	(void)arguments.flags;

	openlog("pam_howdy", 0, LOG_AUTHPRIV);

	char *username = nullptr;
	int   pam_res  = get_username(pamh, &username);
	if (pam_res != PAM_SUCCESS) {
		return pam_res;
	}

	howdy::pam::RuntimeSession runtime_session(kConfiguredConfigPath, kConfiguredUserModelsDir,
	                                           runtime_session_dependencies());

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

	pam_res = run_enabled_check(config, username, runtime_session.user_models_dir());
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
	const bool       existing_auth_token = auth_token_present(pamh);

	howdy::pam::PromptCoordinator coordinator(
	    pamh, workaround, ask_auth_tok, existing_auth_token, prompt_coordinator_dependencies(),
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

	return map_prompt_result(coordinator.run(compare_request), username, config, conv_function);
}
