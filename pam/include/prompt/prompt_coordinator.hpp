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

	struct PromptCoordinatorDependencies {
		void                            *context                           = nullptr;
		SpawnCompareProcessFn            spawn_compare_process             = nullptr;
		WaitForCompareProcessFn          wait_for_compare_process          = nullptr;
		CancelAndReapCompareProcessFn    cancel_and_reap_compare_process   = nullptr;
		InputPromptPreflightFn           input_prompt_preflight            = nullptr;
		CreatePromptSubmitterFn          create_prompt_submitter           = nullptr;
		CreateNativePromptFn             create_native_prompt              = nullptr;
		CreateSecretPromptConversationFn create_secret_prompt_conversation = nullptr;
		RequestAuthTokenFn               request_auth_token                = nullptr;
	};

	enum class PromptCoordinatorDecision : std::uint8_t {
		kHowdyResult,
		kPamResult,
		kPasswordFallback,
		kInvalidDependencies,
		kCompareSpawnFailed,
		kAlreadyRun,
	};

	struct PromptCoordinatorResult {
		PromptCoordinatorDecision decision       = PromptCoordinatorDecision::kInvalidDependencies;
		int                       compare_status = 0;
		int                       pam_status     = PAM_SUCCESS;
	};

	class PromptCoordinator {
	public:
		PromptCoordinator(pam_handle_t *pamh, Workaround workaround, bool ask_auth_tok,
		                  bool existing_auth_token, PromptCoordinatorDependencies dependencies,
		                  std::chrono::steady_clock::duration hard_timeout);

		PromptCoordinator(const PromptCoordinator &)                     = delete;
		auto operator=(const PromptCoordinator &) -> PromptCoordinator & = delete;
		PromptCoordinator(PromptCoordinator &&)                          = delete;
		auto operator=(PromptCoordinator &&) -> PromptCoordinator &      = delete;

		~PromptCoordinator() = default;

		[[nodiscard]] auto Valid() const -> bool;

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
		PromptCoordinatorDependencies             dependencies_;
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
