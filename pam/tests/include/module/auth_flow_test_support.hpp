#pragma once
#include "module/auth_flow.hpp"
#include "pam_test_support.hpp"
#include "protocol/runtime_descriptor_message.hpp"
#include "storage/user_model_status.hpp"
#include "support/scoped_fd.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

#include <security/pam_appl.h>

#include <sys/types.h>

namespace howdy::test::auth_flow {

	using howdy::pam::PamModuleArguments;
	using howdy::pam::PromptSubmitter;

	class ScopedPamHandle {
	public:
		ScopedPamHandle(ScopedPamHandle &&)                     = delete;
		auto operator=(ScopedPamHandle &&) -> ScopedPamHandle & = delete;
		ScopedPamHandle()                                       = default;

		ScopedPamHandle(const ScopedPamHandle &)                     = delete;
		auto operator=(const ScopedPamHandle &) -> ScopedPamHandle & = delete;

		~ScopedPamHandle() {
			if (pamh_ != nullptr) {
				pam_end(pamh_, PAM_SUCCESS);
			}
		}

		auto Start(const struct pam_conv *conversation, const char *username = "test-user") -> int {
			return howdy::test::PamStartForTest("howdy-auth-flow-test", username, conversation,
			                                    &pamh_);
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

	// A real file the fake opener hands out, so the runtime session's descriptor checks pass
	// for the test's own UID (IdentifyDependencies::runtime_owner_uid).
	inline auto FlowDescriptorPath() -> const std::filesystem::path & {
		class DescriptorFile {
		public:
			DescriptorFile() {
				const howdy::native::ScopedFd file(
				    open(path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600));
			}

			DescriptorFile(const DescriptorFile &)                     = delete;
			auto operator=(const DescriptorFile &) -> DescriptorFile & = delete;
			DescriptorFile(DescriptorFile &&)                          = delete;
			auto operator=(DescriptorFile &&) -> DescriptorFile &      = delete;

			~DescriptorFile() {
				(void)unlink(path_.c_str());
			}

			[[nodiscard]] auto Path() const -> const std::filesystem::path & {
				return path_;
			}

		private:
			std::filesystem::path path_ =
			    std::filesystem::current_path() /
			    ("howdy-auth-flow-descriptor-" + std::to_string(getpid()));
		};

		static const DescriptorFile kDescriptorFile;
		return kDescriptorFile.Path();
	}

	struct FlowReadiness {
		howdy::native::UserModelStatus status = howdy::native::UserModelStatus::kNoModel;
		std::string                    error_message;
	};

	// The opener's model state for a readiness result.
	inline auto FlowModelState(const FlowReadiness &readiness)
	    -> howdy::native::runtime_descriptor_message::ModelState {
		using howdy::native::Classify;
		using howdy::native::UserModelOutcome;
		using howdy::native::UserModelStatus;
		using howdy::native::runtime_descriptor_message::ModelState;
		if (readiness.status == UserModelStatus::kInvalidUser) {
			return ModelState::kInvalidUser;
		}
		switch (Classify(readiness.status)) {
			case UserModelOutcome::kOk:
				return ModelState::kReady;
			case UserModelOutcome::kAbsent:
				return ModelState::kMissing;
			default:
				return ModelState::kInvalidStorage;
		}
	}

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
		int                                    in_process_calls = 0;
		int                                    helper_calls     = 0;
		int                                    timeout          = 5;
		uid_t                                  effective_uid    = 0;
		bool                                   open_result      = true;
		howdy::native::RuntimeConfigLoadStatus load_status =
		    howdy::native::RuntimeConfigLoadStatus::kOk;
		int load_error_code = 0;
	};

	struct EligibilityFlowState {
		bool                                ssh = false;
		howdy::pam::runtime::LidStateResult lid{
		    .status = howdy::pam::runtime::LidProbeStatus::kOk,
		    .state  = howdy::pam::runtime::LidState::kOpen,
		};
		FlowReadiness readiness{
		    .status = howdy::native::UserModelStatus::kOk,
		};
		int ssh_calls   = 0;
		int lid_calls   = 0;
		int model_calls = 0;
	};

	struct PromptFlowState {
		int spawn_calls    = 0;
		int compare_status = 0;
	};

	struct EligibilityFlowFixture {
		RuntimeFlowState     runtime;
		EligibilityFlowState eligibility;
		PromptFlowState      prompt;
	};

	// The fake opener: real descriptors for a real file, and the model state the fixture's
	// readiness result calls for. Counted as a model check, which the opener always makes.
	inline auto FlowOpenDescriptors(EligibilityFlowFixture           *fixture,
	                                howdy::pam::RuntimeDescriptorSet *descriptors) -> bool {
		++fixture->eligibility.model_calls;
		if (!fixture->runtime.open_result) {
			return false;
		}
		const auto &path    = FlowDescriptorPath();
		descriptors->config = howdy::native::ScopedFd(open(path.c_str(), O_RDONLY | O_CLOEXEC));
		descriptors->model_state   = FlowModelState(fixture->eligibility.readiness);
		descriptors->model_message = fixture->eligibility.readiness.error_message;
		if (descriptors->model_state ==
		    howdy::native::runtime_descriptor_message::ModelState::kReady) {
			descriptors->model = howdy::native::ScopedFd(open(path.c_str(), O_RDONLY | O_CLOEXEC));
		}
		return true;
	}

	inline auto FlowOpenThroughHelper(void *context, std::string_view username,
	                                  howdy::pam::RuntimeDescriptorSet *descriptors) -> bool {
		(void)username;
		auto *fixture = static_cast<EligibilityFlowFixture *>(context);
		++fixture->runtime.helper_calls;
		return FlowOpenDescriptors(fixture, descriptors);
	}

	inline auto FlowEffectiveUid(void *context) -> uid_t {
		return static_cast<const EligibilityFlowFixture *>(context)->runtime.effective_uid;
	}

	inline auto FlowLoadRuntimeConfig(void *context, int config_fd,
	                                  const std::filesystem::path &path, uid_t owner_uid)
	    -> howdy::native::RuntimeConfigLoadResult {
		(void)config_fd;
		(void)owner_uid;
		auto &state = static_cast<EligibilityFlowFixture *>(context)->runtime;
		++state.load_calls;
		if (state.load_status != howdy::native::RuntimeConfigLoadStatus::kOk) {
			return {
			    .ok            = false,
			    .status        = state.load_status,
			    .path          = path,
			    .config        = std::nullopt,
			    .error_message = "flow runtime configuration failure",
			    .error_code    = state.load_error_code,
			};
		}

		howdy::native::RuntimeConfig config;
		config.core.detection_notice    = state.detection_notice;
		config.core.no_confirmation     = true;
		config.core.abort_if_ssh        = true;
		config.core.abort_if_lid_closed = true;
		config.core.disabled            = state.disabled;
		config.video.timeout            = state.timeout;
		return {
		    .ok            = true,
		    .status        = howdy::native::RuntimeConfigLoadStatus::kOk,
		    .path          = path,
		    .config        = std::move(config),
		    .error_message = {},
		    .error_code    = 0,
		};
	}

	inline auto FlowOpenInProcess(void *context, std::string_view username,
	                              const howdy::pam::RuntimeSessionPaths &paths,
	                              howdy::pam::RuntimeDescriptorSet      *descriptors) -> bool {
		(void)username;
		(void)paths;
		return FlowOpenDescriptors(static_cast<EligibilityFlowFixture *>(context), descriptors);
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

	// NOLINTBEGIN(bugprone-easily-swappable-parameters)
	inline auto FlowSpawnCompare(void *context, const howdy::pam::CompareLaunchRequest &request,
	                             pid_t *child_pid, int *child_pidfd, int *verdict_fd) -> int {
		// NOLINTEND(bugprone-easily-swappable-parameters)
		(void)request;
		auto *state = static_cast<PromptFlowState *>(context);
		++state->spawn_calls;
		*child_pid   = 1;
		*child_pidfd = -1;
		*verdict_fd  = -1;
		return 0;
	}

	// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
	inline auto FlowWaitCompare(void *context, pid_t child_pid, int child_pidfd, int verdict_fd,
	                            std::chrono::steady_clock::time_point      deadline,
	                            void                                      *cancellation_context,
	                            howdy::pam::CompareCancellationRequestedFn cancellation_requested,
	                            int                                        cancellation_fd) -> int {
		(void)cancellation_fd;
		(void)child_pid;
		(void)child_pidfd;
		(void)verdict_fd;
		(void)deadline;
		(void)cancellation_context;
		(void)cancellation_requested;
		return static_cast<const PromptFlowState *>(context)->compare_status;
	}

	// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
	inline void FlowCancelAndReapCompare(void *context, pid_t child_pid, int child_pidfd) noexcept {
		(void)context;
		(void)child_pid;
		(void)child_pidfd;
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
		auto runtime_ops = howdy::pam::RuntimeSessionOperations::Create(
		    fixture, FlowOpenInProcess, FlowOpenThroughHelper, FlowLoadRuntimeConfig,
		    FlowEffectiveUid);
		auto prompt_ops = howdy::pam::PromptCoordinatorOperations::Create(
		    &fixture->prompt, FlowSpawnCompare, FlowWaitCompare, FlowCancelAndReapCompare,
		    FlowInputPromptPreflight, FlowCreatePromptSubmitter, FlowCreateNativePrompt,
		    FlowCreateSecretPromptConversation, FlowRequestAuthToken);
		auto eligibility_ops =
		    howdy::pam::auth_eligibility::AuthenticationEligibilityOperations::Create(
		        &fixture->eligibility, FlowSshSessionPresent, FlowReadLidState);
		if (!runtime_ops.has_value() || !prompt_ops.has_value() || !eligibility_ops.has_value()) {
			std::abort();
		}
		return {
		    .runtime_session    = *std::move(runtime_ops),
		    .prompt_coordinator = *std::move(prompt_ops),
		    .eligibility        = *std::move(eligibility_ops),
		    .runtime_owner_uid  = geteuid(),
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
		if (state == nullptr || num_msg != 1 || messages == nullptr || response == nullptr) {
			return PAM_CONV_ERR;
		}
		const auto *message = std::span(messages, std::size_t{1}).front();
		if (message == nullptr) {
			return PAM_CONV_ERR;
		}

		++state->calls;
		state->last_msg_type = message->msg_style;
		state->last_message  = message->msg == nullptr ? "" : message->msg;
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
