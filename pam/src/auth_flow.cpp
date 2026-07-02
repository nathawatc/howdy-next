#include "common/compare_exit.hpp"
#include "storage/user_model_readiness.hpp"
#ifdef HOWDY_PAM_TESTING
#	include "auth_flow_testing.hpp"
#endif
#include "enter_device.hpp"
#include "main.hpp"
#include "native_prompt_conversation.hpp"
#include "optional_task.hpp"
#include "prompt_workaround.hpp"
#include "runtime_session.hpp"
#include "status_mapping.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <clocale>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <glob.h>
#include <libintl.h>
#include <mutex>
#include <optional>
#include <paths.hpp>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <syslog.h>
#include <tuple>
#include <unistd.h>

#include <security/pam_appl.h>
#include <security/pam_ext.h>
#include <security/pam_modules.h>

#include <sys/types.h>
#include <sys/wait.h>

namespace {

	constexpr auto kPromptRetryDelay =
	    std::chrono::duration<int, std::chrono::milliseconds::period>(100);
	constexpr int kMaxPromptRetries = 5;

	auto S(const char *msg) -> const char * {
		return gettext(msg);
	}

	using howdy::native::CompareExit;

	auto make_wait_exit_status(CompareExit exit_code) -> int {
		return static_cast<int>(exit_code) << 8;
	}

	auto compare_status_is(int status, CompareExit exit_code) -> bool {
		return WIFEXITED(status) && WEXITSTATUS(status) == static_cast<int>(exit_code);
	}

	using ConversationFn = std::function<int(int, const char *)>;

	struct PromptStopResult {
		bool enter_failed   = false;
		bool prompt_stopped = true;
	};

	struct NativePromptCleanupGuard {
		optional_task<std::tuple<int, char *>> *pass_task     = nullptr;
		NativePromptConversation               *native_prompt = nullptr;

		~NativePromptCleanupGuard() {
			if (pass_task == nullptr || native_prompt == nullptr || !pass_task->active()) {
				return;
			}

			try {
				native_prompt->request_abort();
				pass_task->stop();
				native_prompt->restore_original();
			} catch (const std::exception &error) {
				syslog(LOG_CRIT, "Native prompt cleanup failed: %s", error.what());
			} catch (...) {
				syslog(LOG_CRIT, "Native prompt cleanup failed with non-standard exception");
			}
		}
	};

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
		*conv_function                      = [original_conv](int msg_type, const char *msg_str) {
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

	auto howdy_status(char *username, int status, const howdy::native::RuntimeConfig &config,
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

	auto wait_for_compare_process(pid_t child_pid) -> int {
		while (true) {
			int         status      = 0;
			const pid_t wait_result = waitpid(child_pid, &status, 0);
			if (wait_result == child_pid) {
				return status;
			}
			if (wait_result < 0 && errno == EINTR) {
				continue;
			}

			syslog(LOG_ERR, "waitpid failed for compare process: %s (%d)", strerror(errno), errno);
			return make_wait_exit_status(CompareExit::kAbort);
		}
	}

	auto input_prompt_workaround_preflight() -> bool {
		if (euidaccess("/dev/uinput", W_OK | R_OK) != 0) {
			const int access_errno = errno;
			syslog(LOG_ERR, "Input prompt workaround unavailable: %s (%d)", strerror(access_errno),
			       access_errno);
			return false;
		}

		try {
			EnterDevice probe;
		} catch (const std::runtime_error &err) {
			syslog(LOG_ERR, "Input prompt workaround setup failed: %s", err.what());
			return false;
		}

		return true;
	}

	auto request_password_prompt_stop(optional_task<std::tuple<int, char *>> &pass_task,
	                                  const PromptStopPlan                   &plan,
	                                  NativePromptConversation *native_prompt) -> PromptStopResult {
		PromptStopResult result;
		if (!plan.stop_prompt) {
			return result;
		}

		if (plan.abort_prompt && native_prompt != nullptr) {
			native_prompt->request_abort();
		}

		if (plan.send_enter) {
			if (euidaccess("/dev/uinput", W_OK | R_OK) != 0) {
				syslog(LOG_WARNING, "Insufficient permissions to create the fake device");
				result.enter_failed = true;
			} else {
				try {
					EnterDevice enter_device;
					enter_device.send_enter_press();

					int retries = 0;
					for (; retries < kMaxPromptRetries &&
					       pass_task.wait(kPromptRetryDelay) == std::future_status::timeout;
					     retries++) {
						enter_device.send_enter_press();
					}

					if (retries == kMaxPromptRetries && pass_task.wait(std::chrono::milliseconds(
					                                        0)) == std::future_status::timeout) {
						syslog(LOG_WARNING, "Failed to send enter input before the retries limit");
						result.enter_failed = true;
					}
				} catch (const std::runtime_error &err) {
					syslog(LOG_WARNING, "Failed to send enter input: %s", err.what());
					result.enter_failed = true;
				}
			}
		}

		if (plan.send_enter &&
		    pass_task.wait(std::chrono::milliseconds(0)) == std::future_status::timeout) {
			result.prompt_stopped = false;
			return result;
		}

		pass_task.stop();
		return result;
	}

}  // namespace

#ifdef HOWDY_PAM_TESTING
namespace howdy::pam::testing {

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

	auto howdy_status(char *username, int status, const howdy::native::RuntimeConfig &config,
	                  const ConversationFn &conv_function) -> int {
		return ::howdy_status(username, status, config, conv_function);
	}

	auto check_enabled(const howdy::native::RuntimeConfig &config, const char *username,
	                   const std::filesystem::path &user_models_dir) -> int {
		return ::check_enabled(config, username, user_models_dir);
	}

	auto request_password_prompt_stop(optional_task<std::tuple<int, char *>> &pass_task,
	                                  const PromptStopPlan &plan) -> PromptStopResult {
		const auto result = ::request_password_prompt_stop(pass_task, plan, nullptr);
		return PromptStopResult{.enter_failed   = result.enter_failed,
		                        .prompt_stopped = result.prompt_stopped};
	}

	auto wait_for_compare_process(pid_t child_pid) -> int {
		return ::wait_for_compare_process(child_pid);
	}

}  // namespace howdy::pam::testing
#endif

auto identify(pam_handle_t *pamh, int flags, int argc, const char **argv, bool ask_auth_tok)
    -> int {
	(void)flags;

	openlog("pam_howdy", 0, LOG_AUTHPRIV);

	char *username = nullptr;
	int   pam_res  = pam_get_user(pamh, const_cast<const char **>(&username), nullptr);
	if (pam_res != PAM_SUCCESS || username == nullptr || username[0] == '\0') {
		syslog(LOG_ERR, "Failed to get username");
		return pam_res == PAM_SUCCESS ? PAM_USER_UNKNOWN : pam_res;
	}

	howdy::pam::RuntimeSession runtime_session(
	    kConfiguredConfigPath, kConfiguredUserModelsDir,
	    howdy::pam::production_runtime_session_dependencies());

	const auto runtime_result = runtime_session.load_for_user(username);
	if (runtime_result.status == howdy::pam::RuntimeSessionLoadStatus::kPrepareFailed ||
	    runtime_result.status == howdy::pam::RuntimeSessionLoadStatus::kInvalidDependencies ||
	    runtime_result.status == howdy::pam::RuntimeSessionLoadStatus::kAlreadyLoaded) {
		return PAM_SYSTEM_ERR;
	}

	if (!runtime_result.ok()) {
		syslog(LOG_ERR, "%s", runtime_result.config_result.error_message.c_str());
		return PAM_SYSTEM_ERR;
	}
	const auto &config = *runtime_result.config_result.config;

	pam_res = check_enabled(config, username, runtime_session.user_models_dir());
	if (pam_res != PAM_SUCCESS) {
		return pam_res;
	}

	ConversationFn conv_function;
	pam_res = make_conversation(pamh, &conv_function);
	if (pam_res != PAM_SUCCESS) {
		return pam_res;
	}

	setlocale(LC_ALL, "");
	bindtextdomain(GETTEXT_PACKAGE, LOCALEDIR);
	textdomain(GETTEXT_PACKAGE);

	if (config.core.detection_notice) {
		const int notice_result =
		    conv_function(PAM_TEXT_INFO, S("Attempting facial authentication"));
		if (notice_result != PAM_SUCCESS) {
			syslog(LOG_ERR, "Failed to send detection notice");
		}
	}

	const Workaround workaround           = get_pam_workaround(argc, argv);
	Workaround       effective_workaround = workaround;
	const bool       existing_auth_token  = auth_token_present(pamh);

	std::array<char *, 5> args = {
	    const_cast<char *>(kCompareProcessPath), const_cast<char *>("--config"),
	    const_cast<char *>(runtime_session.config_path().c_str()), username, nullptr};
	std::string user_models_env = "HOWDY_USER_MODELS_DIR=" + runtime_session.user_models_dir();
	std::array<char *, 2> runtime_env = {const_cast<char *>(user_models_env.c_str()), nullptr};
	std::array<char *, 1> empty_env   = {nullptr};
	char **compare_env = runtime_session.staged() ? runtime_env.data() : empty_env.data();
	pid_t  child_pid   = -1;

	const int spawn_result =
	    posix_spawn(&child_pid, kCompareProcessPath, nullptr, nullptr, args.data(), compare_env);
	if (spawn_result != 0) {
		syslog(LOG_ERR, "Can't spawn the howdy process: %s (%d)", strerror(spawn_result),
		       spawn_result);
		return PAM_SYSTEM_ERR;
	}

	std::mutex              mutx;
	std::condition_variable convar;
	ConfirmationType        confirmation_type(ConfirmationType::Unset);

	optional_task<int> child_task([&] {
		const int status = wait_for_compare_process(child_pid);
		{
			std::unique_lock<std::mutex> lock(mutx);
			if (confirmation_type == ConfirmationType::Unset) {
				confirmation_type = ConfirmationType::Howdy;
			}
		}
		convar.notify_one();
		return status;
	});
	child_task.activate();

	std::optional<NativePromptConversation> native_prompt;
	if (workaround == Workaround::Native && ask_auth_tok && !existing_auth_token) {
		native_prompt.emplace(pamh);
		if (!native_prompt->available()) {
			syslog(LOG_INFO,
			       "Native prompt conversation unavailable, falling back to input workaround");
			native_prompt.reset();
			effective_workaround = Workaround::Input;
		} else {
			const int install_result = native_prompt->install();
			if (install_result != PAM_SUCCESS) {
				syslog(LOG_WARNING, "Failed to install native prompt conversation: %d",
				       install_result);
				native_prompt.reset();
				effective_workaround = Workaround::Input;
			}
		}
	}

	if (effective_workaround == Workaround::Input && ask_auth_tok && !existing_auth_token &&
	    !input_prompt_workaround_preflight()) {
		syslog(LOG_WARNING,
		       "Input prompt workaround preflight failed; falling back to standard PAM prompt");
		effective_workaround = Workaround::Off;
	}

	const bool ask_pass =
	    effective_workaround == Workaround::Native
	        ? native_prompt.has_value() && !existing_auth_token
	        : should_ask_for_password(ask_auth_tok, effective_workaround, existing_auth_token);

	optional_task<std::tuple<int, char *>> pass_task([&] {
		char     *auth_tok_ptr = nullptr;
		const int auth_result =
		    pam_get_authtok(pamh, PAM_AUTHTOK, const_cast<const char **>(&auth_tok_ptr), nullptr);
		{
			std::unique_lock<std::mutex> lock(mutx);
			if (confirmation_type == ConfirmationType::Unset) {
				confirmation_type = ConfirmationType::Pam;
			}
		}
		convar.notify_one();
		return std::tuple<int, char *>(auth_result, auth_tok_ptr);
	});

	if (ask_pass) {
		pass_task.activate();
	}

	NativePromptCleanupGuard native_cleanup{
	    .pass_task     = &pass_task,
	    .native_prompt = native_prompt ? &*native_prompt : nullptr,
	};

	{
		std::unique_lock<std::mutex> lock(mutx);
		convar.wait(lock, [&] {
			return confirmation_type != ConfirmationType::Unset;
		});
	}

	if (confirmation_type == ConfirmationType::Pam) {
		if (kill(child_pid, SIGTERM) != 0 && errno != ESRCH) {
			syslog(LOG_WARNING, "Failed to terminate compare process: %s (%d)", strerror(errno),
			       errno);
		}
		child_task.stop();
		if (ask_pass) {
			pass_task.stop();
			char *password              = nullptr;
			std::tie(pam_res, password) = pass_task.get();
			(void)password;
			if (pam_res != PAM_SUCCESS) {
				return pam_res;
			}
		}
		return PAM_IGNORE;
	}

	child_task.stop();
	const int status = child_task.get();

	if (WIFEXITED(status) && WEXITSTATUS(status) != EXIT_SUCCESS && ask_pass) {
		pass_task.stop();

		char *password              = nullptr;
		std::tie(pam_res, password) = pass_task.get();
		(void)password;
		if (pam_res != PAM_SUCCESS) {
			return howdy_status(username, status, config, conv_function);
		}

		return PAM_IGNORE;
	}

	const auto stop_plan =
	    plan_prompt_stop(ask_pass, ask_pass && pass_task.ready(), effective_workaround);
	const auto stop_result = request_password_prompt_stop(
	    pass_task, stop_plan, native_prompt ? &*native_prompt : nullptr);
	if (stop_result.enter_failed) {
		send_conversation_message(
		    conv_function, PAM_ERROR_MSG,
		    S("Failed to send Enter press, waiting for user to press it instead"));
	}
	if (!stop_result.prompt_stopped) {
		syslog(LOG_ERR, "Input prompt workaround cancellation failed; waiting for user/password "
		                "prompt to complete");
		pass_task.stop();
	}

	return howdy_status(username, status, config, conv_function);
}
