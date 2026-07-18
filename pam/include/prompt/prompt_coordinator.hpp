#pragma once

#include "module/main.hpp"
#include "module/prompt_workaround.hpp"
#include "prompt/enter_device.hpp"
#include "prompt/native_prompt_conversation.hpp"
#include "prompt/optional_task.hpp"
#include "runtime/compare_launch.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <tuple>

#include <security/pam_appl.h>

#include <sys/types.h>

namespace howdy::pam {
	using SpawnCompareProcessFn = int (*)(void *context, const CompareLaunchRequest &request,
	                                      pid_t *child_pid);

	using WaitForCompareProcessFn = int (*)(void *context, pid_t child_pid,
	                                        std::chrono::steady_clock::time_point deadline);

	using TerminateCompareProcessFn = void (*)(void *context, pid_t child_pid);

	using InputPromptPreflightFn = bool (*)(void *context);

	using CreateEnterDeviceFn = std::unique_ptr<EnterDevice> (*)(void *context);

	using CreateNativePromptFn = std::unique_ptr<NativePrompt> (*)(void         *context,
	                                                               pam_handle_t *pamh);

	using RequestAuthTokenFn = std::tuple<int, const char *> (*)(void *context, pam_handle_t *pamh);

	struct PromptCoordinatorDependencies {
		void                     *context                  = nullptr;
		SpawnCompareProcessFn     spawn_compare_process    = nullptr;
		WaitForCompareProcessFn   wait_for_compare_process = nullptr;
		TerminateCompareProcessFn terminate_compare        = nullptr;
		InputPromptPreflightFn    input_prompt_preflight   = nullptr;
		CreateEnterDeviceFn       create_enter_device      = nullptr;
		CreateNativePromptFn      create_native_prompt     = nullptr;
		RequestAuthTokenFn        request_auth_token       = nullptr;
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
		bool                      prompt_stopped = true;
	};

	struct PromptStopResult {
		bool enter_failed   = false;
		bool prompt_stopped = true;
	};

	__attribute__((visibility("hidden"))) void
	cleanup_native_prompt(optional_task<std::tuple<int, const char *>> *pass_task,
	                      NativePrompt                                 *native_prompt) noexcept;

	__attribute__((visibility("hidden"))) auto
	request_password_prompt_stop(optional_task<std::tuple<int, const char *>> &pass_task,
	                             const PromptStopPlan &plan, NativePrompt *native_prompt,
	                             EnterDevice *enter_device) -> PromptStopResult;

	class PromptCoordinator {
	public:
		PromptCoordinator(pam_handle_t *pamh, Workaround workaround, bool ask_auth_tok,
		                  bool existing_auth_token, PromptCoordinatorDependencies dependencies,
		                  std::chrono::steady_clock::duration hard_timeout);

		PromptCoordinator(const PromptCoordinator &)                     = delete;
		auto operator=(const PromptCoordinator &) -> PromptCoordinator & = delete;
		PromptCoordinator(PromptCoordinator &&)                          = delete;
		auto operator=(PromptCoordinator &&) -> PromptCoordinator &      = delete;

		~PromptCoordinator();

		[[nodiscard]] auto valid() const -> bool;

		auto run(const CompareLaunchRequest  &request,
		         const std::function<void()> &report_input_failure = {}) -> PromptCoordinatorResult;

	private:
		auto start_compare_task(pid_t                                 child_pid,
		                        std::chrono::steady_clock::time_point compare_deadline)
		    -> optional_task<int> &;
		[[nodiscard]] auto configure_prompt_workaround() -> bool;
		void               configure_input_workaround();
		auto start_password_task(bool ask_pass) -> optional_task<std::tuple<int, const char *>> &;

		pam_handle_t                       *pamh_                 = nullptr;
		Workaround                          requested_workaround_ = Workaround::Off;
		bool                                ask_auth_tok_         = false;
		bool                                existing_auth_token_  = false;
		std::chrono::steady_clock::duration hard_timeout_{};
		PromptCoordinatorDependencies       dependencies_;
		std::mutex                          mutex_;
		std::condition_variable             condition_;
		ConfirmationType                    confirmation_type_ = ConfirmationType::Unset;
		std::unique_ptr<NativePrompt>       native_prompt_;
		std::unique_ptr<EnterDevice>        enter_device_;
		std::optional<optional_task<int>>   child_task_;
		std::optional<optional_task<std::tuple<int, const char *>>> pass_task_;
		Workaround effective_workaround_ = Workaround::Off;
		bool       run_started_          = false;
	};

	auto production_prompt_coordinator_dependencies() -> PromptCoordinatorDependencies;

}  // namespace howdy::pam
