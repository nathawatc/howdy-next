#ifndef _GNU_SOURCE
#	define _GNU_SOURCE
#endif

#include "prompt/prompt_coordinator.hpp"

#include "prompt/prompt_submitter.hpp"
#include "prompt/workaround.hpp"
#include "protocol/compare_exit.hpp"
#include "runtime/compare_launch.hpp"
#include "runtime/compare_process.hpp"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <syslog.h>
#include <unistd.h>

#include <security/pam_appl.h>
#include <security/pam_ext.h>

#include <sys/wait.h>

namespace {

	constexpr auto kPromptCompletionGrace = std::chrono::milliseconds(100);

	auto InputWorkaroundAccess() -> int {
		return euidaccess("/dev/uinput", W_OK | R_OK);
	}

	auto InputPromptWorkaroundPreflight() -> bool {
		if (InputWorkaroundAccess() != 0) {
			const int access_errno = errno;
			syslog(LOG_ERR, "Input prompt workaround unavailable: %s (%d)", strerror(access_errno),
			       access_errno);
			return false;
		}

		return true;
	}

	auto InputPromptPreflightDependency(void *context) -> bool {
		(void)context;
		return InputPromptWorkaroundPreflight();
	}

	auto RequestAuthTokenDependency(void *context, pam_handle_t *pamh)
	    -> std::tuple<int, const char *> {
		(void)context;
		const char *auth_tok_ptr = nullptr;
		const int   auth_result  = pam_get_authtok(pamh, PAM_AUTHTOK, &auth_tok_ptr, nullptr);

		return {auth_result, auth_tok_ptr};
	}

	auto CreatePromptSubmitterDependency(void *context)
	    -> std::unique_ptr<howdy::pam::PromptSubmitter> {
		(void)context;
		return howdy::pam::CreateUinputPromptSubmitter();
	}

	auto CreateNativePromptDependency(void *context, pam_handle_t *pamh)
	    -> std::unique_ptr<NativePrompt> {
		(void)context;
		return std::make_unique<NativePromptConversation>(pamh);
	}

	auto CreateSecretPromptConversationDependency(void *context, pam_handle_t *pamh,
	                                              howdy::pam::SecretPromptObserver observer)
	    -> std::unique_ptr<howdy::pam::SecretPromptConversation> {
		(void)context;
		return std::make_unique<howdy::pam::ObservedPromptConversation>(pamh, observer);
	}

}  // namespace

namespace howdy::pam {

	PromptCoordinator::PromptCoordinator(pam_handle_t *pamh, Workaround workaround,
	                                     bool ask_auth_tok, bool existing_auth_token,
	                                     PromptCoordinatorDependencies       dependencies,
	                                     std::chrono::steady_clock::duration hard_timeout)
	    : pamh_(pamh)
	    , requested_workaround_(workaround)
	    , ask_auth_tok_(ask_auth_tok)
	    , existing_auth_token_(existing_auth_token)
	    , hard_timeout_(hard_timeout)
	    , dependencies_(dependencies)
	    , effective_workaround_(workaround) {}

	auto PromptCoordinator::Valid() const -> bool {
		return dependencies_.spawn_compare_process != nullptr &&
		       dependencies_.wait_for_compare_process != nullptr &&
		       dependencies_.input_prompt_preflight != nullptr &&
		       dependencies_.create_prompt_submitter != nullptr &&
		       dependencies_.create_native_prompt != nullptr &&
		       dependencies_.create_secret_prompt_conversation != nullptr &&
		       dependencies_.request_auth_token != nullptr &&
		       hard_timeout_ > std::chrono::steady_clock::duration::zero();
	}

	auto PromptCoordinator::CancellationRequested(void *context) -> bool {
		auto            &coordinator = *static_cast<PromptCoordinator *>(context);
		std::scoped_lock lock(coordinator.mutex_);
		return coordinator.state_.cancellation_requested;
	}

	auto PromptCoordinator::SecretPromptBegin(void *context) noexcept -> SecretPromptGeneration {
		auto                  &coordinator = *static_cast<PromptCoordinator *>(context);
		SecretPromptGeneration generation  = 0;
		{
			std::scoped_lock lock(coordinator.mutex_);
			if (coordinator.state_.password_call_entered &&
			    !coordinator.state_.password_call_returned &&
			    !coordinator.state_.shutdown_requested) {
				if (coordinator.state_.submission == PromptSubmissionState::kClaimed) {
					coordinator.state_.submission         = PromptSubmissionState::kPending;
					coordinator.state_.claimed_generation = 0;
				}
				generation = coordinator.state_.secret_prompt_generation ==
				                     std::numeric_limits<SecretPromptGeneration>::max()
				                 ? 1
				                 : coordinator.state_.secret_prompt_generation + 1;
				coordinator.state_.secret_prompt_generation = generation;
				coordinator.state_.secret_prompt_active     = true;
			}
		}
		coordinator.condition_.notify_all();
		return generation;
	}

	void PromptCoordinator::SecretPromptEnd(void                  *context,
	                                        SecretPromptGeneration generation) noexcept {
		auto &coordinator = *static_cast<PromptCoordinator *>(context);
		{
			std::scoped_lock lock(coordinator.mutex_);
			if (generation == 0 || generation != coordinator.state_.secret_prompt_generation) {
				return;
			}
			coordinator.state_.secret_prompt_active = false;
			if (coordinator.state_.submission == PromptSubmissionState::kClaimed &&
			    coordinator.state_.claimed_generation == generation) {
				coordinator.state_.submission         = PromptSubmissionState::kPending;
				coordinator.state_.claimed_generation = 0;
			}
		}
		coordinator.condition_.notify_all();
	}

	auto PromptCoordinator::WaitForCompare(
	    pid_t child_pid, std::chrono::steady_clock::time_point compare_deadline) noexcept -> int {
		int status = static_cast<int>(howdy::native::CompareExit::kAbort) << 8;
		try {
			status = dependencies_.wait_for_compare_process(
			    dependencies_.context, child_pid, compare_deadline, this, CancellationRequested);
		} catch (const std::exception &error) {
			syslog(LOG_ERR, "Compare wait failed: %s", error.what());
			compare_process::CancelAndReap(child_pid);
		} catch (...) {
			syslog(LOG_ERR, "Compare wait failed with non-standard exception");
			compare_process::CancelAndReap(child_pid);
		}
		return status;
	}

	auto PromptCoordinator::PublishCompareCompletion(int status) -> SuccessAction {
		std::unique_lock<std::mutex> lock(mutex_);
		state_.compare_status    = status;
		state_.compare_succeeded = WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS;
		if (state_.first_completion == FirstCompletion::kNone) {
			state_.first_completion = FirstCompletion::kCompare;
		}
		condition_.notify_all();
		if (state_.first_completion != FirstCompletion::kCompare || !state_.compare_succeeded ||
		    state_.password_call_returned || state_.shutdown_requested) {
			return SuccessAction::kNone;
		}
		if (effective_workaround_ == Workaround::kNative && native_prompt_ != nullptr) {
			return SuccessAction::kAbortNative;
		}
		if (effective_workaround_ != Workaround::kInput ||
		    state_.submission != PromptSubmissionState::kPending) {
			return SuccessAction::kNone;
		}

		condition_.wait(lock, [this] -> bool {
			return state_.secret_prompt_active || state_.password_call_returned ||
			       state_.shutdown_requested;
		});
		if (!state_.secret_prompt_active || state_.password_call_returned ||
		    state_.submission != PromptSubmissionState::kPending || state_.shutdown_requested) {
			return SuccessAction::kNone;
		}
		state_.submission         = PromptSubmissionState::kClaimed;
		state_.claimed_generation = state_.secret_prompt_generation;
		return SuccessAction::kSubmitPrompt;
	}

	void PromptCoordinator::RequestNativeAbort() noexcept {
		try {
			native_prompt_->RequestAbort();
		} catch (const std::exception &error) {
			syslog(LOG_WARNING, "Native prompt abort failed: %s", error.what());
		} catch (...) {
			syslog(LOG_WARNING, "Native prompt abort failed with non-standard exception");
		}
	}

	auto PromptCoordinator::WaitForPromptSubmissionClaim() -> bool {
		std::unique_lock<std::mutex> lock(mutex_);
		condition_.wait(lock, [this] -> bool {
			return state_.password_call_returned || state_.shutdown_requested ||
			       state_.first_completion != FirstCompletion::kCompare ||
			       !state_.compare_succeeded ||
			       state_.submission == PromptSubmissionState::kSubmitting ||
			       state_.submission == PromptSubmissionState::kFinished ||
			       (state_.submission == PromptSubmissionState::kPending &&
			        state_.secret_prompt_active);
		});
		if (state_.password_call_returned || state_.shutdown_requested ||
		    state_.first_completion != FirstCompletion::kCompare || !state_.compare_succeeded ||
		    state_.submission != PromptSubmissionState::kPending || !state_.secret_prompt_active) {
			return false;
		}
		state_.submission         = PromptSubmissionState::kClaimed;
		state_.claimed_generation = state_.secret_prompt_generation;
		return true;
	}

	auto PromptCoordinator::SubmitPromptAndRecordResult() noexcept -> PromptSubmissionResult {
		{
			std::scoped_lock lock(mutex_);
			if (state_.first_completion != FirstCompletion::kCompare || !state_.compare_succeeded ||
			    !state_.secret_prompt_active || state_.password_call_returned ||
			    state_.submission != PromptSubmissionState::kClaimed ||
			    state_.claimed_generation != state_.secret_prompt_generation ||
			    state_.shutdown_requested) {
				if (state_.submission == PromptSubmissionState::kClaimed) {
					state_.submission         = PromptSubmissionState::kPending;
					state_.claimed_generation = 0;
				}
				const bool retry = state_.first_completion == FirstCompletion::kCompare &&
				                   state_.compare_succeeded && !state_.password_call_returned &&
				                   !state_.shutdown_requested &&
				                   state_.submission == PromptSubmissionState::kPending;
				return retry ? PromptSubmissionResult::kRetry : PromptSubmissionResult::kStop;
			}
			// This Claimed -> Submitting transition linearizes prompt submission against password
			// return. Whichever transition acquires mutex_ first wins; no external I/O holds
			// mutex_.
			state_.submission = PromptSubmissionState::kSubmitting;
		}

		bool submission_failed = false;
		try {
			if (prompt_submitter_ == nullptr) {
				submission_failed = true;
			} else {
				prompt_submitter_->SubmitPrompt();
			}
		} catch (const std::exception &error) {
			syslog(LOG_WARNING, "Input prompt submission failed: %s", error.what());
			submission_failed = true;
		} catch (...) {
			syslog(LOG_WARNING, "Input prompt submission failed with non-standard exception");
			submission_failed = true;
		}
		std::unique_lock<std::mutex> lock(mutex_);
		state_.submission = PromptSubmissionState::kFinished;
		if (!submission_failed) {
			condition_.wait_for(lock, kPromptCompletionGrace, [this] -> bool {
				return state_.password_call_returned || state_.shutdown_requested;
			});
		}
		const bool prompt_failed = submission_failed || !state_.password_call_returned;
		lock.unlock();
		if (prompt_failed) {
			syslog(LOG_ERR,
			       "Input prompt workaround cancellation failed; waiting for user/password "
			       "prompt to complete");
		}
		return PromptSubmissionResult::kStop;
	}

	void PromptCoordinator::SubmitPromptForGenerations() noexcept {
		while (true) {
			const auto result = SubmitPromptAndRecordResult();
			if (result != PromptSubmissionResult::kRetry || !WaitForPromptSubmissionClaim()) {
				return;
			}
		}
	}

	void PromptCoordinator::CompareWorker(
	    pid_t child_pid, std::chrono::steady_clock::time_point compare_deadline) noexcept {
		const int           status = WaitForCompare(child_pid, compare_deadline);
		const SuccessAction action = PublishCompareCompletion(status);
		if (action == SuccessAction::kAbortNative) {
			RequestNativeAbort();
		} else if (action == SuccessAction::kSubmitPrompt) {
			SubmitPromptForGenerations();
		}
	}

	void PromptCoordinator::DisableNativeWorkaround(bool unavailable) {
		const bool fallback_to_input = requested_workaround_ == Workaround::kNativeInput;
		if (unavailable) {
			syslog(LOG_INFO,
			       fallback_to_input
			           ? "Native prompt conversation unavailable, falling back to input workaround"
			           : "Native prompt conversation unavailable, disabling prompt workaround");
		}
		effective_workaround_ = fallback_to_input ? Workaround::kInput : Workaround::kOff;
		native_prompt_.reset();
	}

	void PromptCoordinator::ConfigureNativeWorkaround() {
		try {
			native_prompt_ = dependencies_.create_native_prompt(dependencies_.context, pamh_);
		} catch (const std::exception &error) {
			syslog(LOG_WARNING, "Native prompt conversation setup failed: %s", error.what());
		} catch (...) {
			syslog(LOG_WARNING,
			       "Native prompt conversation setup failed with non-standard exception");
		}

		if (native_prompt_ == nullptr || !native_prompt_->Available()) {
			DisableNativeWorkaround(true);
			return;
		}

		const int install_result = native_prompt_->Install();
		if (install_result == PAM_SUCCESS) {
			effective_workaround_ = Workaround::kNative;
			return;
		}
		syslog(LOG_WARNING, "Failed to install native prompt conversation: %d", install_result);
		DisableNativeWorkaround(false);
	}

	auto PromptCoordinator::ConfigurePromptWorkaround() -> bool {
		const bool wants_native_prompt = requested_workaround_ == Workaround::kNative ||
		                                 requested_workaround_ == Workaround::kNativeInput;
		if (wants_native_prompt && ask_auth_tok_ && !existing_auth_token_) {
			ConfigureNativeWorkaround();
		}

		ConfigureInputWorkaround();
		return effective_workaround_ == Workaround::kNative
		           ? native_prompt_ != nullptr && !existing_auth_token_
		           : ShouldAskForPassword(ask_auth_tok_, effective_workaround_,
		                                  existing_auth_token_);
	}

	void PromptCoordinator::ConfigureInputWorkaround() {
		if (effective_workaround_ != Workaround::kInput || !ask_auth_tok_ || existing_auth_token_) {
			return;
		}

		if (!dependencies_.input_prompt_preflight(dependencies_.context)) {
			syslog(LOG_WARNING, "Input prompt workaround preflight failed; falling back to "
			                    "standard PAM prompt");
			effective_workaround_ = Workaround::kOff;
			return;
		}

		try {
			prompt_submitter_ = dependencies_.create_prompt_submitter(dependencies_.context);
			if (prompt_submitter_ == nullptr) {
				syslog(LOG_ERR,
				       "Input prompt workaround setup failed: submission backend unavailable");
				effective_workaround_ = Workaround::kOff;
			}
		} catch (const std::exception &err) {
			syslog(LOG_ERR, "Input prompt workaround setup failed: %s", err.what());
			effective_workaround_ = Workaround::kOff;
		} catch (...) {
			syslog(LOG_ERR, "Input prompt workaround setup failed with non-standard exception");
			effective_workaround_ = Workaround::kOff;
		}
		if (effective_workaround_ != Workaround::kInput) {
			return;
		}

		try {
			secret_prompt_conversation_ = dependencies_.create_secret_prompt_conversation(
			    dependencies_.context, pamh_,
			    {.context = this, .begin = SecretPromptBegin, .end = SecretPromptEnd});
			if (secret_prompt_conversation_ == nullptr ||
			    !secret_prompt_conversation_->Available() ||
			    secret_prompt_conversation_->Install() != PAM_SUCCESS) {
				syslog(LOG_ERR, "Input prompt observation setup failed");
				secret_prompt_conversation_.reset();
				prompt_submitter_.reset();
				effective_workaround_ = Workaround::kOff;
			}
		} catch (const std::exception &error) {
			syslog(LOG_ERR, "Input prompt observation setup failed: %s", error.what());
			secret_prompt_conversation_.reset();
			prompt_submitter_.reset();
			effective_workaround_ = Workaround::kOff;
		} catch (...) {
			syslog(LOG_ERR, "Input prompt observation setup failed with non-standard exception");
			secret_prompt_conversation_.reset();
			prompt_submitter_.reset();
			effective_workaround_ = Workaround::kOff;
		}
	}

	auto PromptCoordinator::RestorePromptConversation() noexcept -> ConversationRestoreResult {
		auto result = ConversationRestoreResult::kOriginalRestored;
		if (secret_prompt_conversation_ != nullptr) {
			result = secret_prompt_conversation_->RestoreOriginal();
		} else if (native_prompt_ != nullptr) {
			result = native_prompt_->RestoreOriginal();
		}
		if (result == ConversationRestoreResult::kOriginalRestored) {
			return result;
		}
		syslog(LOG_CRIT, result == ConversationRestoreResult::kFailClosedInstalled
		                     ? "PAM conversation restoration failed; fail-closed callback installed"
		                     : "PAM conversation restoration unsafe; callback context quarantined");
		return result;
	}

	void PromptCoordinator::InitializeRunState(bool ask_pass) {
		std::scoped_lock lock(mutex_);
		if (!ask_pass) {
			return;
		}
		state_.submission = effective_workaround_ == Workaround::kInput
		                        ? PromptSubmissionState::kPending
		                        : PromptSubmissionState::kNotApplicable;
	}

	void PromptCoordinator::PublishPasswordCallEntered() {
		std::scoped_lock lock(mutex_);
		state_.password_call_entered = true;
	}

	void PromptCoordinator::CleanupSpawnedChild(
	    pid_t child_pid, std::chrono::steady_clock::time_point compare_deadline) noexcept {
		{
			std::scoped_lock lock(mutex_);
			state_.cancellation_requested = true;
			state_.shutdown_requested     = true;
		}
		condition_.notify_all();
		CompareWorker(child_pid, compare_deadline);
		(void)RestorePromptConversation();
	}

	auto PromptCoordinator::RequestPassword() noexcept -> int {
		try {
			const auto [result, password] =
			    dependencies_.request_auth_token(dependencies_.context, pamh_);
			(void)password;
			return result;
		} catch (const std::exception &error) {
			syslog(LOG_ERR, "Password request failed: %s", error.what());
		} catch (...) {
			syslog(LOG_ERR, "Password request failed with non-standard exception");
		}
		return PAM_SYSTEM_ERR;
	}

	void PromptCoordinator::PublishPasswordCallReturned() {
		{
			std::scoped_lock lock(mutex_);
			state_.password_call_returned = true;
			state_.secret_prompt_active   = false;
			if (state_.submission == PromptSubmissionState::kClaimed) {
				state_.submission         = PromptSubmissionState::kPending;
				state_.claimed_generation = 0;
			}
			if (state_.first_completion == FirstCompletion::kNone) {
				state_.first_completion       = FirstCompletion::kPassword;
				state_.cancellation_requested = true;
			}
		}
		condition_.notify_all();
	}

	auto PromptCoordinator::BuildResult(bool ask_pass, int pam_result) -> PromptCoordinatorResult {
		std::scoped_lock        lock(mutex_);
		PromptCoordinatorResult result{
		    .compare_status = state_.compare_status,
		    .pam_status     = pam_result,
		};
		if (state_.first_completion == FirstCompletion::kPassword) {
			result.decision = PromptCoordinatorDecision::kPamResult;
		} else if (!state_.compare_succeeded && ask_pass) {
			result.decision = PromptCoordinatorDecision::kPasswordFallback;
		} else {
			result.decision = PromptCoordinatorDecision::kHowdyResult;
		}
		return result;
	}

	auto PromptCoordinator::Run(const CompareLaunchRequest &request) -> PromptCoordinatorResult {
		if (run_started_) {
			return {.decision = PromptCoordinatorDecision::kAlreadyRun};
		}
		run_started_ = true;

		if (!Valid()) {
			return {.decision = PromptCoordinatorDecision::kInvalidDependencies};
		}

		const auto compare_deadline = std::chrono::steady_clock::now() + hard_timeout_;
		pid_t      child_pid        = -1;
		const int  spawn_result =
		    dependencies_.spawn_compare_process(dependencies_.context, request, &child_pid);

		if (spawn_result != 0 || child_pid <= 0) {
			if (spawn_result != 0) {
				syslog(LOG_ERR, "Unable to start Howdy: %s (%d)", strerror(spawn_result),
				       spawn_result);
			} else {
				syslog(LOG_ERR, "Unable to start Howdy: invalid child pid");
			}
			return {
			    .decision = PromptCoordinatorDecision::kCompareSpawnFailed,
			};
		}

		bool ask_pass = false;
		try {
			ask_pass = ConfigurePromptWorkaround();
		} catch (const std::exception &error) {
			syslog(LOG_ERR, "Prompt workaround setup failed: %s", error.what());
			CleanupSpawnedChild(child_pid, compare_deadline);
			return {.decision = PromptCoordinatorDecision::kInvalidDependencies};
		} catch (...) {
			syslog(LOG_ERR, "Prompt workaround setup failed with non-standard exception");
			CleanupSpawnedChild(child_pid, compare_deadline);
			return {.decision = PromptCoordinatorDecision::kInvalidDependencies};
		}

		InitializeRunState(ask_pass);

		std::thread child_thread;
		try {
			child_thread =
			    std::thread(&PromptCoordinator::CompareWorker, this, child_pid, compare_deadline);
		} catch (const std::exception &error) {
			syslog(LOG_ERR, "Failed to start compare wait worker: %s", error.what());
			CleanupSpawnedChild(child_pid, compare_deadline);
			return {.decision = PromptCoordinatorDecision::kInvalidDependencies};
		}
		int pam_result = PAM_SUCCESS;
		if (ask_pass) {
			PublishPasswordCallEntered();
			pam_result = RequestPassword();
			PublishPasswordCallReturned();
		}

		child_thread.join();
		const bool terminal_restore_failed =
		    native_prompt_ != nullptr && native_prompt_->TerminalRestoreFailed();
		const auto restore_result = RestorePromptConversation();
		if (terminal_restore_failed ||
		    restore_result != ConversationRestoreResult::kOriginalRestored) {
			pam_result = PAM_SYSTEM_ERR;
		}

		auto result = BuildResult(ask_pass, pam_result);
		if (terminal_restore_failed ||
		    restore_result != ConversationRestoreResult::kOriginalRestored) {
			result.decision   = PromptCoordinatorDecision::kPamResult;
			result.pam_status = PAM_SYSTEM_ERR;
		}
		return result;
	}

	auto ProductionPromptCoordinatorDependencies() -> PromptCoordinatorDependencies {
		return PromptCoordinatorDependencies{
		    .spawn_compare_process             = compare_process::Spawn,
		    .wait_for_compare_process          = compare_process::Wait,
		    .input_prompt_preflight            = InputPromptPreflightDependency,
		    .create_prompt_submitter           = CreatePromptSubmitterDependency,
		    .create_native_prompt              = CreateNativePromptDependency,
		    .create_secret_prompt_conversation = CreateSecretPromptConversationDependency,
		    .request_auth_token                = RequestAuthTokenDependency,
		};
	}

}  // namespace howdy::pam
