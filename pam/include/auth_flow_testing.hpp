#ifndef HOWDY_PAM_AUTH_FLOW_TESTING_HPP
#define HOWDY_PAM_AUTH_FLOW_TESTING_HPP

#ifdef HOWDY_PAM_TESTING

#	include "common/fd_io.hpp"
#	include "optional_task.hpp"
#	include "prompt_coordinator.hpp"
#	include "prompt_workaround.hpp"
#	include "runtime_session.hpp"

#	include <filesystem>
#	include <functional>
#	include <cstddef>
#	include <string>
#	include <tuple>

#	include <security/pam_appl.h>

#	include <sys/types.h>

namespace howdy::native {
	struct RuntimeConfig;
}

namespace howdy::pam::testing {

	using ConversationFn         = std::function<int(int, const char *)>;
	using AuthHelperOutputReader = howdy::native::BoundedReadResult (*)(int, std::size_t);

	auto send_conversation_message(const ConversationFn &conv_function, int msg_type,
	                               const std::string &message) -> void;
	auto make_conversation(pam_handle_t *pamh, ConversationFn *conv_function) -> int;
	auto auth_token_present(pam_handle_t *pamh) -> bool;
	auto howdy_error(int status, const ConversationFn &conv_function) -> int;
	auto howdy_status(char *username, int status, const howdy::native::RuntimeConfig &config,
	                  const ConversationFn &conv_function) -> int;
	auto check_enabled(const howdy::native::RuntimeConfig &config, const char *username,
	                   const std::filesystem::path &user_models_dir) -> int;

	using CheckEnabledFn = int (*)(void *context, const howdy::native::RuntimeConfig &config,
	                               const char                  *username,
	                               const std::filesystem::path &user_models_dir);

	struct IdentifyDependencies {
		void                         *context = nullptr;
		RuntimeSessionDependencies    runtime_session;
		PromptCoordinatorDependencies prompt_coordinator;
		CheckEnabledFn                check_enabled = nullptr;
	};

	auto set_identify_dependencies(const IdentifyDependencies &dependencies) -> void;
	auto reset_identify_dependencies() -> void;

	struct PromptStopResult {
		bool enter_failed   = false;
		bool prompt_stopped = true;
	};

	auto request_password_prompt_stop(optional_task<std::tuple<int, char *>> &pass_task,
	                                  const PromptStopPlan &plan) -> PromptStopResult;
	auto wait_for_compare_process(pid_t child_pid) -> int;
	auto auth_helper_output_limit() -> std::size_t;
	auto set_auth_helper_output_reader(AuthHelperOutputReader reader) -> AuthHelperOutputReader;

	struct AuthHelperOutput {
		std::string config_path;
		std::string user_models_dir;
		bool        valid = false;
	};

	auto read_fd_to_string(int fd) -> std::string;
	auto read_auth_helper_output(pid_t child_pid, int output_fd, std::string *output) -> bool;
	auto parse_auth_helper_output(const std::string &output) -> AuthHelperOutput;
	auto wait_for_helper_process(pid_t child_pid) -> int;

}  // namespace howdy::pam::testing

#endif

#endif  // HOWDY_PAM_AUTH_FLOW_TESTING_HPP
