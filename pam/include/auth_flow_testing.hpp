#ifndef HOWDY_PAM_AUTH_FLOW_TESTING_HPP
#define HOWDY_PAM_AUTH_FLOW_TESTING_HPP

#ifdef HOWDY_PAM_TESTING

#	include "optional_task.hpp"
#	include "prompt_workaround.hpp"

#	include <filesystem>
#	include <functional>
#	include <string>
#	include <tuple>

#	include <security/pam_appl.h>

#	include <sys/types.h>

namespace howdy::native {
	struct RuntimeConfig;
}

namespace howdy::pam::testing {

	using ConversationFn = std::function<int(int, const char *)>;

	auto send_conversation_message(const ConversationFn &conv_function, int msg_type,
	                               const std::string &message) -> void;
	auto make_conversation(pam_handle_t *pamh, ConversationFn *conv_function) -> int;
	auto auth_token_present(pam_handle_t *pamh) -> bool;
	auto howdy_error(int status, const ConversationFn &conv_function) -> int;
	auto howdy_status(char *username, int status, const howdy::native::RuntimeConfig &config,
	                  const ConversationFn &conv_function) -> int;
	auto check_enabled(const howdy::native::RuntimeConfig &config, const char *username,
	                   const std::filesystem::path &user_models_dir) -> int;

	struct PromptStopResult {
		bool enter_failed   = false;
		bool prompt_stopped = true;
	};

	auto request_password_prompt_stop(optional_task<std::tuple<int, char *>> &pass_task,
	                                  const PromptStopPlan &plan) -> PromptStopResult;
	auto wait_for_compare_process(pid_t child_pid) -> int;
	auto read_fd_to_string(int fd) -> std::string;
	auto helper_output_value(const std::string &output, const std::string &key) -> std::string;
	auto wait_for_helper_process(pid_t child_pid) -> int;

}  // namespace howdy::pam::testing

#endif

#endif  // HOWDY_PAM_AUTH_FLOW_TESTING_HPP
