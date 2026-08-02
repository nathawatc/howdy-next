#pragma once

#include "module/main.hpp"
#include "prompt/conversation_restore.hpp"
#include "prompt/enter_device.hpp"
#include "prompt/native_prompt_conversation.hpp"
#include "prompt/observed_prompt_conversation.hpp"
#include "runtime/compare_launch.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
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

	using InputPromptPreflightFn = bool (*)(void *context);

	using CreateEnterDeviceFn = std::unique_ptr<EnterDevice> (*)(void *context);

	using CreateNativePromptFn = std::unique_ptr<NativePrompt> (*)(void         *context,
	                                                               pam_handle_t *pamh);
	using CreateSecretPromptConversationFn = std::unique_ptr<SecretPromptConversation> (*)(
	    void *context, pam_handle_t *pamh, SecretPromptObserver observer);
	using RequestAuthTokenFn = std::tuple<int, const char *> (*)(void *context, pam_handle_t *pamh);

	struct PromptCoordinatorDependencies {
		void                            *context                           = nullptr;
		SpawnCompareProcessFn            spawn_compare_process             = nullptr;
		WaitForCompareProcessFn          wait_for_compare_process          = nullptr;
		InputPromptPreflightFn           input_prompt_preflight            = nullptr;
		CreateEnterDeviceFn              create_enter_device               = nullptr;
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

		[[nodiscard]] auto valid() const -> bool;

		auto run(const CompareLaunchRequest  &request,
		         const std::function<void()> &report_input_failure = {}) -> PromptCoordinatorResult;

	private:
		friend class PromptCoordinatorTestAccess;

		enum class FirstCompletion : std::uint8_t {
			kNone,
			kPassword,
			kCompare
		};
		enum class EnterState : std::uint8_t {
			kNotApplicable,
			kPending,
			kClaimed,
			kEmitting,
			kFinished,
		};
		enum class SuccessAction : std::uint8_t {
			kNone,
			kAbortNative,
			kSendEnter,
		};
		enum class EnterEmissionResult : std::uint8_t {
			kStop,
			kRetry,
		};

		struct State {
			FirstCompletion        first_completion         = FirstCompletion::kNone;
			EnterState             enter                    = EnterState::kNotApplicable;
			SecretPromptGeneration secret_prompt_generation = 0;
			SecretPromptGeneration claimed_generation       = 0;
			int                    compare_status           = 0;
			bool                   compare_succeeded        = false;
			bool                   password_call_entered    = false;
			bool                   password_call_returned   = false;
			bool                   secret_prompt_active     = false;
			bool                   cancellation_requested   = false;
			bool                   deferred_failure_notice  = false;
			bool                   shutdown_requested       = false;
		};

		static auto cancellation_requested(void *context) -> bool;
		static auto secret_prompt_begin(void *context) noexcept -> SecretPromptGeneration;
		static void secret_prompt_end(void *context, SecretPromptGeneration generation) noexcept;
		[[nodiscard]] auto
		wait_for_compare(pid_t                                 child_pid,
		                 std::chrono::steady_clock::time_point compare_deadline) noexcept -> int;
		[[nodiscard]] auto publish_compare_completion(int status) -> SuccessAction;
		void               request_native_abort() noexcept;
		[[nodiscard]] auto wait_for_enter_claim() -> bool;
		[[nodiscard]] auto send_enter_and_record_result() noexcept -> EnterEmissionResult;
		void               send_enter_for_prompt_generations() noexcept;
		void compare_worker(pid_t                                 child_pid,
		                    std::chrono::steady_clock::time_point compare_deadline) noexcept;
		void configure_native_workaround();
		void disable_native_workaround(bool unavailable);
		[[nodiscard]] auto configure_prompt_workaround() -> bool;
		void               configure_input_workaround();
		void               initialize_run_state(bool ask_pass);
		void               publish_password_call_entered();
		void cleanup_spawned_child(pid_t                                 child_pid,
		                           std::chrono::steady_clock::time_point compare_deadline) noexcept;
		[[nodiscard]] auto request_password() noexcept -> int;
		void               publish_password_call_returned();
		[[nodiscard]] auto build_result(bool ask_pass, int pam_result) -> PromptCoordinatorResult;
		void report_deferred_failure(const std::function<void()> &report_input_failure) noexcept;
		[[nodiscard]] auto restore_prompt_conversation() noexcept -> ConversationRestoreResult;

		pam_handle_t                             *pamh_                 = nullptr;
		Workaround                                requested_workaround_ = Workaround::Off;
		bool                                      ask_auth_tok_         = false;
		bool                                      existing_auth_token_  = false;
		std::chrono::steady_clock::duration       hard_timeout_{};
		PromptCoordinatorDependencies             dependencies_;
		std::mutex                                mutex_;
		std::condition_variable                   condition_;
		std::unique_ptr<NativePrompt>             native_prompt_;
		std::unique_ptr<SecretPromptConversation> secret_prompt_conversation_;
		std::unique_ptr<EnterDevice>              enter_device_;
		State                                     state_;
		Workaround                                effective_workaround_ = Workaround::Off;
		bool                                      run_started_          = false;
	};

	auto production_prompt_coordinator_dependencies() -> PromptCoordinatorDependencies;

}  // namespace howdy::pam
