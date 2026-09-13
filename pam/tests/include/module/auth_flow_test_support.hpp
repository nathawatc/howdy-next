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

		auto Start(const struct pam_conv *conversation, const char *username = "test-user") -> int {
			return pam_start("howdy-auth-flow-test", username, conversation, &pamh_);
		}

		[[nodiscard]] auto Get() const -> pam_handle_t * {
			return pamh_;
		}

	private:
		pam_handle_t *pamh_ = nullptr;
	};

	enum class ResponseMode : std::uint8_t {
		kNone,
		kEmpty,
		kSecret,
	};

	struct ConversationState {
		int          result        = PAM_SUCCESS;
		int          calls         = 0;
		int          last_msg_type = 0;
		std::string  last_message;
		ResponseMode response_mode = ResponseMode::kNone;
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

	inline auto FlowPrepareRuntime(void *context, std::string_view username,
	                               howdy::pam::PreparedRuntimeFiles *prepared) -> bool {
		(void)username;
		auto *state = static_cast<RuntimeFlowState *>(context);
		++state->prepare_calls;
		if (!state->prepare_result) {
			return false;
		}
		const auto root = howdy::native::auth_helper_protocol::PreparedRuntimeGenerationDir(
		    howdy::native::auth_helper_protocol::PreparedRuntimeRoot(), getuid(),
		    howdy::native::auth_helper_protocol::RuntimeGenerationSlot::kSlot0);
		std::array<int, 2> lease_pipe = {-1, -1};
		if (pipe2(lease_pipe.data(), O_CLOEXEC) != 0) {
			return false;
		}
		(void)close(lease_pipe[1]);
		*prepared = {
		    .root_dir    = root,
		    .config_path = howdy::native::auth_helper_protocol::PreparedConfigPath(root).string(),
		    .user_models_dir =
		        howdy::native::auth_helper_protocol::PreparedUserModelsDir(root).string(),
		    .lease_fd = lease_pipe[0],
		};
		return true;
	}

	inline auto FlowLoadRuntimeConfig(void *context, const std::filesystem::path &path)
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

	inline auto FlowEffectiveUid(void *context) -> uid_t {
		auto *state = static_cast<RuntimeFlowState *>(context);
		return state->effective_uid;
	}

	inline auto FlowSshSessionPresent(void *context, pam_handle_t *pamh) -> bool {
		(void)pamh;
		auto *state = static_cast<EligibilityFlowState *>(context);
		++state->ssh_calls;
		return state->ssh;
	}

	inline auto FlowReadLidState(void *context) -> howdy::pam::runtime::LidStateResult {
		auto *state = static_cast<EligibilityFlowState *>(context);
		++state->lid_calls;
		return state->lid;
	}

	inline auto FlowCheckModelReadiness(void *context, const std::filesystem::path &models_dir,
	                                    const char *username)
	    -> howdy::native::UserModelReadinessResult {
		(void)models_dir;
		(void)username;
		auto *state = static_cast<EligibilityFlowState *>(context);
		++state->model_calls;
		return state->readiness;
	}

	inline auto FlowSpawnCompare(void *context, const howdy::pam::CompareLaunchRequest &request,
	                             pid_t *child_pid) -> int {
		(void)request;
		auto *state = static_cast<PromptFlowState *>(context);
		++state->spawn_calls;
		*child_pid = 1;
		return 0;
	}

	inline auto FlowWaitCompare(void *context, pid_t child_pid,
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

	inline void FlowCancelAndReapCompare(void *context, pid_t child_pid) noexcept {
		(void)context;
		(void)child_pid;
	}

	inline auto FlowInputPromptPreflight(void *context) -> bool {
		(void)context;
		return true;
	}

	inline auto FlowCreatePromptSubmitter(void *context) -> std::unique_ptr<PromptSubmitter> {
		(void)context;
		return nullptr;
	}

	inline auto FlowCreateNativePrompt(void *context, pam_handle_t *pamh)
	    -> std::unique_ptr<NativePrompt> {
		(void)context;
		(void)pamh;
		return nullptr;
	}

	inline auto FlowCreateSecretPromptConversation(void *context, pam_handle_t *pamh,
	                                               howdy::pam::SecretPromptObserver observer)
	    -> std::unique_ptr<howdy::pam::SecretPromptConversation> {
		(void)context;
		(void)pamh;
		(void)observer;
		return nullptr;
	}

	inline auto FlowRequestAuthToken(void *context, pam_handle_t *pamh)
	    -> std::tuple<int, const char *> {
		(void)context;
		(void)pamh;
		return {PAM_SUCCESS, nullptr};
	}

	inline auto MakeEligibilityFlowDependencies(EligibilityFlowFixture *fixture)
	    -> howdy::pam::auth_flow::IdentifyDependencies {
		return {
		    .runtime_session =
		        {
		            .context             = &fixture->runtime,
		            .prepare_runtime     = FlowPrepareRuntime,
		            .load_runtime_config = FlowLoadRuntimeConfig,
		            .effective_uid       = FlowEffectiveUid,
		        },
		    .prompt_coordinator =
		        {
		            .context                           = &fixture->prompt,
		            .spawn_compare_process             = FlowSpawnCompare,
		            .wait_for_compare_process          = FlowWaitCompare,
		            .cancel_and_reap_compare_process   = FlowCancelAndReapCompare,
		            .input_prompt_preflight            = FlowInputPromptPreflight,
		            .create_prompt_submitter           = FlowCreatePromptSubmitter,
		            .create_native_prompt              = FlowCreateNativePrompt,
		            .create_secret_prompt_conversation = FlowCreateSecretPromptConversation,
		            .request_auth_token                = FlowRequestAuthToken,
		        },
		    .eligibility =
		        {
		            .context               = &fixture->eligibility,
		            .ssh_session_present   = FlowSshSessionPresent,
		            .read_lid_state        = FlowReadLidState,
		            .check_model_readiness = FlowCheckModelReadiness,
		        },
		};
	}

	inline auto IdentifyForTest(void *context, pam_handle_t *pamh, PamModuleArguments arguments,
	                            bool ask_auth_tok) -> int {
		const auto *dependencies =
		    static_cast<const howdy::pam::auth_flow::IdentifyDependencies *>(context);
		return howdy::pam::auth_flow::IdentifyWithDependencies(pamh, arguments, ask_auth_tok,
		                                                       *dependencies);
	}

	inline auto TestConversation(int num_msg, const struct pam_message **messages,
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

		if (state->response_mode != ResponseMode::kNone) {
			*response = static_cast<struct pam_response *>(calloc(1, sizeof(struct pam_response)));
			if (*response == nullptr) {
				return PAM_BUF_ERR;
			}
			if (state->response_mode == ResponseMode::kSecret) {
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
