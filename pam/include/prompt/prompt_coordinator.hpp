#pragma once

#include "prompt/conversation_restore.hpp"
#include "prompt/native_prompt_conversation.hpp"
#include "prompt/observed_prompt_conversation.hpp"
#include "prompt/prompt_submitter.hpp"
#include "prompt/workaround.hpp"
#include "runtime/compare_launch.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <tuple>

#include <security/pam_appl.h>

#include <sys/types.h>

namespace howdy::pam {
	class PromptCoordinatorTestAccess;

	using SpawnCompareProcessFn = int (*)(void *context, const CompareLaunchRequest &request,
	                                      pid_t *child_pid);

	using WaitForCompareProcessFn = int (*)(void *context, pid_t child_pid,
	                                        std::chrono::steady_clock::time_point deadline,
	                                        void                          *cancellation_context,
	                                        CompareCancellationRequestedFn cancellation_requested);
	using CancelAndReapCompareProcessFn = void (*)(void *context, pid_t child_pid) noexcept;

	using InputPromptPreflightFn = bool (*)(void *context);

	using CreatePromptSubmitterFn = std::unique_ptr<PromptSubmitter> (*)(void *context);

	using CreateNativePromptFn = std::unique_ptr<NativePrompt> (*)(void         *context,
	                                                               pam_handle_t *pamh);
	using CreateSecretPromptConversationFn = std::unique_ptr<SecretPromptConversation> (*)(
	    void *context, pam_handle_t *pamh, SecretPromptObserver observer);
	using RequestAuthTokenFn = std::tuple<int, const char *> (*)(void *context, pam_handle_t *pamh);

	class PromptCoordinatorOperations {
	public:
		static auto Create(void *context, SpawnCompareProcessFn spawn_compare_process,
		                   WaitForCompareProcessFn          wait_for_compare_process,
		                   CancelAndReapCompareProcessFn    cancel_and_reap_compare_process,
		                   InputPromptPreflightFn           input_prompt_preflight,
		                   CreatePromptSubmitterFn          create_prompt_submitter,
		                   CreateNativePromptFn             create_native_prompt,
		                   CreateSecretPromptConversationFn create_secret_prompt_conversation,
		                   RequestAuthTokenFn               request_auth_token)
		    -> std::optional<PromptCoordinatorOperations> {
			if (spawn_compare_process == nullptr || wait_for_compare_process == nullptr ||
			    cancel_and_reap_compare_process == nullptr || input_prompt_preflight == nullptr ||
			    create_prompt_submitter == nullptr || create_native_prompt == nullptr ||
			    create_secret_prompt_conversation == nullptr || request_auth_token == nullptr) {
				return std::nullopt;
			}
			return PromptCoordinatorOperations(
			    context, spawn_compare_process, wait_for_compare_process,
			    cancel_and_reap_compare_process, input_prompt_preflight, create_prompt_submitter,
			    create_native_prompt, create_secret_prompt_conversation, request_auth_token);
		}

		[[nodiscard]] auto SpawnCompareProcess(const CompareLaunchRequest &request,
		                                       pid_t                      *child_pid) const -> int {
			return spawn_compare_process_(context_, request, child_pid);
		}

		[[nodiscard]] auto
		WaitForCompareProcess(pid_t child_pid, std::chrono::steady_clock::time_point deadline,
		                      void                          *cancellation_context,
		                      CompareCancellationRequestedFn cancellation_requested) const -> int {
			return wait_for_compare_process_(context_, child_pid, deadline, cancellation_context,
			                                 cancellation_requested);
		}

		void CancelAndReapCompareProcess(pid_t child_pid) const noexcept {
			cancel_and_reap_compare_process_(context_, child_pid);
		}

		[[nodiscard]] auto InputPromptPreflight() const -> bool {
			return input_prompt_preflight_(context_);
		}

		[[nodiscard]] auto CreatePromptSubmitter() const -> std::unique_ptr<PromptSubmitter> {
			return create_prompt_submitter_(context_);
		}

		[[nodiscard]] auto CreateNativePrompt(pam_handle_t *pamh) const
		    -> std::unique_ptr<NativePrompt> {
			return create_native_prompt_(context_, pamh);
		}

		[[nodiscard]] auto CreateSecretPromptConversation(pam_handle_t        *pamh,
		                                                  SecretPromptObserver observer) const
		    -> std::unique_ptr<SecretPromptConversation> {
			return create_secret_prompt_conversation_(context_, pamh, observer);
		}

		[[nodiscard]] auto RequestAuthToken(pam_handle_t *pamh) const
		    -> std::tuple<int, const char *> {
			return request_auth_token_(context_, pamh);
		}

	private:
		PromptCoordinatorOperations(
		    void *context, SpawnCompareProcessFn spawn_compare_process,
		    WaitForCompareProcessFn          wait_for_compare_process,
		    CancelAndReapCompareProcessFn    cancel_and_reap_compare_process,
		    InputPromptPreflightFn           input_prompt_preflight,
		    CreatePromptSubmitterFn          create_prompt_submitter,
		    CreateNativePromptFn             create_native_prompt,
		    CreateSecretPromptConversationFn create_secret_prompt_conversation,
		    RequestAuthTokenFn               request_auth_token)
		    : context_(context)
		    , spawn_compare_process_(spawn_compare_process)
		    , wait_for_compare_process_(wait_for_compare_process)
		    , cancel_and_reap_compare_process_(cancel_and_reap_compare_process)
		    , input_prompt_preflight_(input_prompt_preflight)
		    , create_prompt_submitter_(create_prompt_submitter)
		    , create_native_prompt_(create_native_prompt)
		    , create_secret_prompt_conversation_(create_secret_prompt_conversation)
		    , request_auth_token_(request_auth_token) {}

		void                            *context_                           = nullptr;
		SpawnCompareProcessFn            spawn_compare_process_             = nullptr;
		WaitForCompareProcessFn          wait_for_compare_process_          = nullptr;
		CancelAndReapCompareProcessFn    cancel_and_reap_compare_process_   = nullptr;
		InputPromptPreflightFn           input_prompt_preflight_            = nullptr;
		CreatePromptSubmitterFn          create_prompt_submitter_           = nullptr;
		CreateNativePromptFn             create_native_prompt_              = nullptr;
		CreateSecretPromptConversationFn create_secret_prompt_conversation_ = nullptr;
		RequestAuthTokenFn               request_auth_token_                = nullptr;
	};

	class PromptCoordinatorTimeout {
	public:
		static auto Create(std::chrono::steady_clock::duration duration)
		    -> std::optional<PromptCoordinatorTimeout> {
			if (duration <= std::chrono::steady_clock::duration::zero()) {
				return std::nullopt;
			}
			return PromptCoordinatorTimeout(duration);
		}

		[[nodiscard]] auto Duration() const noexcept -> std::chrono::steady_clock::duration {
			return duration_;
		}

	private:
		explicit PromptCoordinatorTimeout(std::chrono::steady_clock::duration duration)
		    : duration_(duration) {}

		std::chrono::steady_clock::duration duration_{};
	};

	enum class PromptCoordinatorDecision : std::uint8_t {
		kHowdyResult,
		kPamResult,
		kPasswordFallback,
		kCompareSpawnFailed,
		kAlreadyRun,
	};

	struct PromptCoordinatorResult {
		PromptCoordinatorDecision decision       = PromptCoordinatorDecision::kPamResult;
		int                       compare_status = 0;
		int                       pam_status     = PAM_SYSTEM_ERR;
	};

	class PromptCoordinator {
	public:
		PromptCoordinator(pam_handle_t *pamh, Workaround workaround, bool ask_auth_tok,
		                  bool existing_auth_token, PromptCoordinatorOperations operations,
		                  PromptCoordinatorTimeout hard_timeout);

		PromptCoordinator(const PromptCoordinator &)                     = delete;
		auto operator=(const PromptCoordinator &) -> PromptCoordinator & = delete;
		PromptCoordinator(PromptCoordinator &&)                          = delete;
		auto operator=(PromptCoordinator &&) -> PromptCoordinator &      = delete;

		~PromptCoordinator() = default;

		auto Run(const CompareLaunchRequest &request) -> PromptCoordinatorResult;

	private:
		friend class PromptCoordinatorTestAccess;

		enum class FirstCompletion : std::uint8_t {
			kNone,
			kPassword,
			kCompare
		};
		enum class PromptSubmissionState : std::uint8_t {
			kNotApplicable,
			kPending,
			kClaimed,
			kSubmitting,
			kFinished,
		};
		enum class SuccessAction : std::uint8_t {
			kNone,
			kAbortNative,
			kSubmitPrompt,
		};
		enum class PromptSubmissionResult : std::uint8_t {
			kStop,
			kRetry,
		};

		struct State {
			FirstCompletion        first_completion         = FirstCompletion::kNone;
			PromptSubmissionState  submission               = PromptSubmissionState::kNotApplicable;
			SecretPromptGeneration secret_prompt_generation = 0;
			SecretPromptGeneration claimed_generation       = 0;
			int                    compare_status           = 0;
			bool                   compare_succeeded        = false;
			bool                   password_call_entered    = false;
			bool                   password_call_returned   = false;
			bool                   secret_prompt_active     = false;
			bool                   cancellation_requested   = false;
			bool                   shutdown_requested       = false;
		};

		static auto CancellationRequested(void *context) -> bool;
		static auto SecretPromptBegin(void *context) noexcept -> SecretPromptGeneration;
		static void SecretPromptEnd(void *context, SecretPromptGeneration generation) noexcept;
		[[nodiscard]] auto
		WaitForCompare(pid_t                                 child_pid,
		               std::chrono::steady_clock::time_point compare_deadline) noexcept -> int;
		[[nodiscard]] auto PublishCompareCompletion(int status) -> SuccessAction;
		void               RequestNativeAbort() noexcept;
		[[nodiscard]] auto WaitForPromptSubmissionClaim() -> bool;
		[[nodiscard]] auto SubmitPromptAndRecordResult() noexcept -> PromptSubmissionResult;
		void               SubmitPromptForGenerations() noexcept;
		void CompareWorker(pid_t                                 child_pid,
		                   std::chrono::steady_clock::time_point compare_deadline) noexcept;
		void ConfigureNativeWorkaround();
		void DisableNativeWorkaround(bool unavailable);
		[[nodiscard]] auto ConfigurePromptWorkaround() -> bool;
		void               ConfigureInputWorkaround();
		void               InitializeRunState(bool ask_pass);
		void               PublishPasswordCallEntered();
		void CleanupSpawnedChild(pid_t                                 child_pid,
		                         std::chrono::steady_clock::time_point compare_deadline) noexcept;
		[[nodiscard]] auto RequestPassword() noexcept -> int;
		void               PublishPasswordCallReturned();
		[[nodiscard]] auto BuildResult(bool ask_pass, int pam_result) -> PromptCoordinatorResult;
		[[nodiscard]] auto RestorePromptConversation() noexcept -> ConversationRestoreResult;

		pam_handle_t                             *pamh_                 = nullptr;
		Workaround                                requested_workaround_ = Workaround::kOff;
		bool                                      ask_auth_tok_         = false;
		bool                                      existing_auth_token_  = false;
		std::chrono::steady_clock::duration       hard_timeout_{};
		PromptCoordinatorOperations               operations_;
		std::mutex                                mutex_;
		std::condition_variable                   condition_;
		std::unique_ptr<NativePrompt>             native_prompt_;
		std::unique_ptr<SecretPromptConversation> secret_prompt_conversation_;
		std::unique_ptr<PromptSubmitter>          prompt_submitter_;
		State                                     state_;
		Workaround                                effective_workaround_ = Workaround::kOff;
		bool                                      run_started_          = false;
	};

}  // namespace howdy::pam
