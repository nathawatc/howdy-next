#pragma once

#include "module/auth_flow.hpp"
#include "protocol/auth_helper_protocol.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <unistd.h>
#include <utility>

#include <security/pam_appl.h>

#include <sys/types.h>

namespace howdy::test::auth_flow {

	using howdy::pam::PamModuleArguments;
	using howdy::pam::PromptSubmitter;

	class ScopedPamHandle {
	public:
		ScopedPamHandle() = default;

		ScopedPamHandle(const ScopedPamHandle &)                     = delete;
		auto operator=(const ScopedPamHandle &) -> ScopedPamHandle & = delete;

		~ScopedPamHandle() {
			if (pamh_ != nullptr) {
				pam_end(pamh_, PAM_SUCCESS);
			}
		}

		auto start(const struct pam_conv *conversation, const char *username = "test-user") -> int {
			return pam_start("howdy-auth-flow-test", username, conversation, &pamh_);
		}

		[[nodiscard]] auto get() const -> pam_handle_t * {
			return pamh_;
		}

	private:
		pam_handle_t *pamh_ = nullptr;
	};

	enum class ResponseMode : std::uint8_t {
		None,
		Empty,
		Secret,
	};

	struct ConversationState {
		int          result        = PAM_SUCCESS;
		int          calls         = 0;
		int          last_msg_type = 0;
		std::string  last_message;
		ResponseMode response_mode = ResponseMode::None;
	};

	struct RuntimeFlowState {
		bool                                   disabled         = false;
		bool                                   detection_notice = true;
		int                                    load_calls       = 0;
		int                                    prepare_calls    = 0;
		int                                    timeout          = 5;
		uid_t                                  effective_uid    = 0;
		bool                                   prepare_result   = false;
		howdy::native::RuntimeConfigLoadStatus initial_load_status =
		    howdy::native::RuntimeConfigLoadStatus::kOk;
		howdy::native::RuntimeConfigLoadStatus staged_load_status =
		    howdy::native::RuntimeConfigLoadStatus::kOk;
		int initial_error_code = 0;
		int staged_error_code  = 0;
	};

	struct EligibilityFlowState {
		bool                                ssh = false;
		howdy::pam::runtime::LidStateResult lid{
		    .status = howdy::pam::runtime::LidProbeStatus::kOk,
		    .state  = howdy::pam::runtime::LidState::kOpen,
		};
		howdy::native::UserModelReadinessResult readiness{
		    .status = howdy::native::UserModelStatus::kOk,
		};
		int ssh_calls   = 0;
		int lid_calls   = 0;
		int model_calls = 0;
	};

	struct PromptFlowState {
		int spawn_calls = 0;
	};

	struct EligibilityFlowFixture {
		RuntimeFlowState     runtime;
		EligibilityFlowState eligibility;
		PromptFlowState      prompt;
	};

	inline auto flow_prepare_runtime(void *context, std::string_view username,
	                                 howdy::pam::PreparedRuntimeFiles *prepared) -> bool {
		(void)username;
		auto *state = static_cast<RuntimeFlowState *>(context);
		++state->prepare_calls;
		if (!state->prepare_result) {
			return false;
		}
		const auto root = howdy::native::auth_helper_protocol::prepared_runtime_generation_dir(
		    howdy::native::auth_helper_protocol::prepared_runtime_root(), getuid(),
		    howdy::native::auth_helper_protocol::RuntimeGenerationSlot::kSlot0);
		std::array<int, 2> lease_pipe = {-1, -1};
		if (pipe2(lease_pipe.data(), O_CLOEXEC) != 0) {
			return false;
		}
		(void)close(lease_pipe[1]);
		*prepared = {
		    .root_dir    = root,
		    .config_path = howdy::native::auth_helper_protocol::prepared_config_path(root).string(),
		    .user_models_dir =
		        howdy::native::auth_helper_protocol::prepared_user_models_dir(root).string(),
		    .lease_fd = lease_pipe[0],
		};
		return true;
	}

	inline auto flow_load_runtime_config(void *context, const std::filesystem::path &path)
	    -> howdy::native::RuntimeConfigLoadResult {
		auto *state = static_cast<RuntimeFlowState *>(context);
		++state->load_calls;
		const bool staged_load = state->load_calls > 1;
		const auto status = staged_load ? state->staged_load_status : state->initial_load_status;
		const int  error_code = staged_load ? state->staged_error_code : state->initial_error_code;
		if (status != howdy::native::RuntimeConfigLoadStatus::kOk) {
			return {
			    .ok            = false,
			    .status        = status,
			    .path          = path,
			    .config        = std::nullopt,
			    .error_message = "flow runtime configuration failure",
			    .error_code    = error_code,
			};
		}

		howdy::native::RuntimeConfig config;
		config.core.detection_notice    = state->detection_notice;
		config.core.no_confirmation     = true;
		config.core.abort_if_ssh        = true;
		config.core.abort_if_lid_closed = true;
		config.core.disabled            = state->disabled;
		config.video.timeout            = state->timeout;
		return {
		    .ok            = true,
		    .status        = howdy::native::RuntimeConfigLoadStatus::kOk,
		    .path          = path,
		    .config        = std::move(config),
		    .error_message = {},
		    .error_code    = 0,
		};
	}

	inline auto flow_effective_uid(void *context) -> uid_t {
		auto *state = static_cast<RuntimeFlowState *>(context);
		return state->effective_uid;
	}

	inline auto flow_ssh_session_present(void *context, pam_handle_t *pamh) -> bool {
		(void)pamh;
		auto *state = static_cast<EligibilityFlowState *>(context);
		++state->ssh_calls;
		return state->ssh;
	}

	inline auto flow_read_lid_state(void *context) -> howdy::pam::runtime::LidStateResult {
		auto *state = static_cast<EligibilityFlowState *>(context);
		++state->lid_calls;
		return state->lid;
	}

	inline auto flow_check_model_readiness(void *context, const std::filesystem::path &models_dir,
	                                       const char *username)
	    -> howdy::native::UserModelReadinessResult {
		(void)models_dir;
		(void)username;
		auto *state = static_cast<EligibilityFlowState *>(context);
		++state->model_calls;
		return state->readiness;
	}

	inline auto flow_spawn_compare(void *context, const howdy::pam::CompareLaunchRequest &request,
	                               pid_t *child_pid) -> int {
		(void)request;
		auto *state = static_cast<PromptFlowState *>(context);
		++state->spawn_calls;
		*child_pid = 1;
		return 0;
	}

	inline auto flow_wait_compare(void *context, pid_t child_pid,
	                              std::chrono::steady_clock::time_point      deadline,
	                              void                                      *cancellation_context,
	                              howdy::pam::CompareCancellationRequestedFn cancellation_requested)
	    -> int {
		(void)context;
		(void)child_pid;
		(void)deadline;
		(void)cancellation_context;
		(void)cancellation_requested;
		return 0;
	}

	inline auto flow_input_prompt_preflight(void *context) -> bool {
		(void)context;
		return true;
	}

	inline auto flow_create_prompt_submitter(void *context) -> std::unique_ptr<PromptSubmitter> {
		(void)context;
		return nullptr;
	}

	inline auto flow_create_native_prompt(void *context, pam_handle_t *pamh)
	    -> std::unique_ptr<NativePrompt> {
		(void)context;
		(void)pamh;
		return nullptr;
	}

	inline auto flow_create_secret_prompt_conversation(void *context, pam_handle_t *pamh,
	                                                   howdy::pam::SecretPromptObserver observer)
	    -> std::unique_ptr<howdy::pam::SecretPromptConversation> {
		(void)context;
		(void)pamh;
		(void)observer;
		return nullptr;
	}

	inline auto flow_request_auth_token(void *context, pam_handle_t *pamh)
	    -> std::tuple<int, const char *> {
		(void)context;
		(void)pamh;
		return {PAM_SUCCESS, nullptr};
	}

	inline auto make_eligibility_flow_dependencies(EligibilityFlowFixture *fixture)
	    -> howdy::pam::auth_flow::IdentifyDependencies {
		return {
		    .runtime_session =
		        {
		            .context             = &fixture->runtime,
		            .prepare_runtime     = flow_prepare_runtime,
		            .load_runtime_config = flow_load_runtime_config,
		            .effective_uid       = flow_effective_uid,
		        },
		    .prompt_coordinator =
		        {
		            .context                           = &fixture->prompt,
		            .spawn_compare_process             = flow_spawn_compare,
		            .wait_for_compare_process          = flow_wait_compare,
		            .input_prompt_preflight            = flow_input_prompt_preflight,
		            .create_prompt_submitter           = flow_create_prompt_submitter,
		            .create_native_prompt              = flow_create_native_prompt,
		            .create_secret_prompt_conversation = flow_create_secret_prompt_conversation,
		            .request_auth_token                = flow_request_auth_token,
		        },
		    .eligibility =
		        {
		            .context               = &fixture->eligibility,
		            .ssh_session_present   = flow_ssh_session_present,
		            .read_lid_state        = flow_read_lid_state,
		            .check_model_readiness = flow_check_model_readiness,
		        },
		};
	}

	inline auto identify_for_test(void *context, pam_handle_t *pamh, PamModuleArguments arguments,
	                              bool ask_auth_tok) -> int {
		const auto *dependencies =
		    static_cast<const howdy::pam::auth_flow::IdentifyDependencies *>(context);
		return howdy::pam::auth_flow::identify_with_dependencies(pamh, arguments, ask_auth_tok,
		                                                         *dependencies);
	}

	inline auto test_conversation(int num_msg, const struct pam_message **messages,
	                              struct pam_response **response, void *appdata_ptr) -> int {
		auto *state = static_cast<ConversationState *>(appdata_ptr);
		if (state == nullptr || num_msg != 1 || messages == nullptr || messages[0] == nullptr ||
		    response == nullptr) {
			return PAM_CONV_ERR;
		}

		++state->calls;
		state->last_msg_type = messages[0]->msg_style;
		state->last_message  = messages[0]->msg == nullptr ? "" : messages[0]->msg;
		*response            = nullptr;

		if (state->response_mode != ResponseMode::None) {
			*response = static_cast<struct pam_response *>(calloc(1, sizeof(struct pam_response)));
			if (*response == nullptr) {
				return PAM_BUF_ERR;
			}
			if (state->response_mode == ResponseMode::Secret) {
				constexpr const char *secret = "temporary-secret";
				const auto            length = std::strlen(secret) + 1;
				(*response)->resp            = static_cast<char *>(std::malloc(length));
				if ((*response)->resp == nullptr) {
					free(*response);
					*response = nullptr;
					return PAM_BUF_ERR;
				}
				std::memcpy((*response)->resp, secret, length);
			}
		}

		return state->result;
	}

}  // namespace howdy::test::auth_flow
