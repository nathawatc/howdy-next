#pragma once

#include "main.hpp"
#include "native_prompt_conversation.hpp"
#include "optional_task.hpp"

#include <condition_variable>
#include <mutex>
#include <optional>
#include <tuple>

#include <security/pam_appl.h>

#include <sys/types.h>

namespace howdy::pam {

	using WaitForCompareProcessFn = int (*)(void *context, pid_t child_pid);

	using TerminateCompareProcessFn = void (*)(void *context, pid_t child_pid);

	using InputPromptPreflightFn = bool (*)(void *context);

	using RequestAuthTokenFn = std::tuple<int, char *> (*)(void *context, pam_handle_t *pamh);

	struct PromptCoordinatorDependencies {
		void                     *context                  = nullptr;
		WaitForCompareProcessFn   wait_for_compare_process = nullptr;
		TerminateCompareProcessFn terminate_compare        = nullptr;
		InputPromptPreflightFn    input_prompt_preflight   = nullptr;
		RequestAuthTokenFn        request_auth_token       = nullptr;
	};

	enum class PromptCoordinatorDecision {
		kHowdyResult,
		kPamResult,
		kPasswordFallback,
		kInvalidDependencies,
		kAlreadyRun,
	};

	struct PromptCoordinatorResult {
		PromptCoordinatorDecision decision       = PromptCoordinatorDecision::kInvalidDependencies;
		int                       compare_status = 0;
		int                       pam_status     = PAM_SUCCESS;
		bool                      enter_failed   = false;
		bool                      prompt_stopped = true;
	};

	class PromptCoordinator {
	public:
		PromptCoordinator(pam_handle_t *pamh, Workaround workaround, bool ask_auth_tok,
		                  bool existing_auth_token, PromptCoordinatorDependencies dependencies);

		PromptCoordinator(const PromptCoordinator &)                     = delete;
		auto operator=(const PromptCoordinator &) -> PromptCoordinator & = delete;
		PromptCoordinator(PromptCoordinator &&)                          = delete;
		auto operator=(PromptCoordinator &&) -> PromptCoordinator &      = delete;

		~PromptCoordinator();

		[[nodiscard]] auto valid() const -> bool;

		auto run(pid_t compare_child_pid) -> PromptCoordinatorResult;

	private:
		pam_handle_t                           *pamh_                 = nullptr;
		Workaround                              requested_workaround_ = Workaround::Off;
		bool                                    ask_auth_tok_         = false;
		bool                                    existing_auth_token_  = false;
		PromptCoordinatorDependencies           dependencies_;
		std::mutex                              mutex_;
		std::condition_variable                 condition_;
		ConfirmationType                        confirmation_type_ = ConfirmationType::Unset;
		std::optional<NativePromptConversation> native_prompt_;
		std::optional<optional_task<int>>       child_task_;
		std::optional<optional_task<std::tuple<int, char *>>> pass_task_;
		Workaround effective_workaround_ = Workaround::Off;
		bool       run_started_          = false;
	};

	auto production_prompt_coordinator_dependencies() -> PromptCoordinatorDependencies;

}  // namespace howdy::pam
