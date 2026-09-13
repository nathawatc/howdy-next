#pragma once

#include "prompt/prompt_coordinator_test_access.hpp"
#include "prompt/workaround.hpp"
#include "protocol/compare_exit.hpp"
#include "support/process_test_support.hpp"
#include "test_support.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>

#include <security/pam_appl.h>

namespace howdy::test::prompt_coordinator {

	using howdy::test::expect;

	using howdy::native::CompareExit;
	using howdy::pam::PromptCoordinator;
	using howdy::pam::PromptCoordinatorDecision;
	using howdy::pam::PromptCoordinatorDependencies;
	using howdy::pam::PromptSubmitter;
	using howdy::pam::Workaround;
	using namespace std::chrono_literals;

	struct FakeContext {
		std::thread::id                                 run_thread;
		std::thread::id                                 wait_thread;
		std::thread::id                                 auth_token_thread;
		std::thread::id                                 submission_thread;
		std::thread::id                                 native_create_thread;
		std::thread::id                                 native_install_thread;
		std::thread::id                                 native_restore_thread;
		std::thread::id                                 native_abort_thread;
		std::chrono::milliseconds                       token_delay{0};
		PromptCoordinator                              *coordinator_for_submission = nullptr;
		howdy::pam::SecretPromptObserver                secret_prompt_observer{};
		std::string                                     spawned_config_path;
		std::string                                     spawned_username;
		std::string                                     spawned_user_models_dir;
		std::mutex                                      reap_mutex;
		std::mutex                                      token_mutex;
		std::mutex                                      submission_mutex;
		std::mutex                                      native_prompt_mutex;
		std::condition_variable                         reap_condition;
		std::condition_variable                         token_condition;
		std::condition_variable                         submission_condition;
		std::condition_variable                         native_prompt_condition;
		std::atomic<int>                                spawn_calls{0};
		std::atomic<pid_t>                              spawned_pid{-1};
		std::atomic<int>                                wait_calls{0};
		std::atomic<pid_t>                              waited_pid{-1};
		std::atomic<int>                                last_wait_status{0};
		std::atomic<int>                                terminate_calls{0};
		std::atomic<pid_t>                              terminated_pid{-1};
		std::atomic<int>                                cleanup_calls{0};
		std::atomic<pid_t>                              cleaned_pid{-1};
		std::atomic<int>                                preflight_calls{0};
		std::atomic<int>                                prompt_submitter_constructions{0};
		std::atomic<int>                                prompt_submissions{0};
		std::atomic<int>                                auth_token_calls{0};
		int                                             token_result          = PAM_SUCCESS;
		int                                             native_install_result = PAM_SUCCESS;
		std::atomic<int>                                native_abort_calls{0};
		std::atomic<int>                                native_restore_calls{0};
		std::atomic<int>                                secret_restore_calls{0};
		int                                             prompt_master_fd = -1;
		std::atomic<int>                                original_conversation_calls{0};
		int                                             spawn_result   = 0;
		pid_t                                           next_child_pid = -1;
		std::atomic<bool>                               auth_token_active{false};
		bool                                            block_token_until_release          = false;
		bool                                            use_real_auth_token                = false;
		bool                                            block_before_conversation          = false;
		bool                                            complete_without_conversation      = false;
		bool                                            hold_reaped_until_cancel           = false;
		bool                                            token_waits_for_reap               = false;
		bool                                            throw_compare_wait                 = false;
		bool                                            child_reaped_by_wait               = false;
		bool                                            before_conversation                = false;
		bool                                            release_conversation               = false;
		bool                                            release_token                      = false;
		bool                                            preflight_result                   = true;
		bool                                            fail_prompt_submitter_construction = false;
		bool                                            return_null_prompt_submitter       = false;
		bool                                            throw_native_prompt                = false;
		bool                                            throw_native_prompt_unknown        = false;
		bool                                            throw_secret_prompt                = false;
		bool                                            throw_secret_prompt_unknown        = false;
		bool                                            return_null_secret_prompt          = false;
		bool                                            secret_prompt_available            = true;
		int                                             secret_prompt_install_result = PAM_SUCCESS;
		bool                                            throw_auth_token             = false;
		bool                                            throw_auth_token_unknown     = false;
		bool                                            fail_prompt_submission       = false;
		bool                                            release_token_on_submission  = false;
		bool                                            block_submission_after_start = false;
		std::atomic<bool>                               token_returned{false};
		std::atomic<howdy::pam::SecretPromptGeneration> active_prompt_generation{0};
		std::atomic<bool>                               submission_ready{false};
		std::atomic<bool>                     submission_started_before_password_return{false};
		std::atomic<bool>                     submission_finished{false};
		bool                                  release_submission                       = false;
		bool                                  request_native_prompt                    = false;
		bool                                  complete_native_prompt                   = false;
		bool                                  native_available                         = true;
		bool                                  native_terminal_restore_failed           = false;
		bool                                  native_terminal_restore_failure_on_abort = false;
		howdy::pam::ConversationRestoreResult native_restore_result =
		    howdy::pam::ConversationRestoreResult::kOriginalRestored;
		howdy::pam::ConversationRestoreResult secret_restore_result =
		    howdy::pam::ConversationRestoreResult::kOriginalRestored;
		std::atomic<bool> native_prompt_seen{false};
		std::atomic<bool> native_prompt_installed{false};
		std::atomic<bool> native_prompt_input_sent{false};
		std::atomic<bool> native_prompt_completed{false};
		std::atomic<bool> pam_completion_observed_by_waiter{false};
		bool              suppress_secret_prompt = false;
		bool              spawned_staged_runtime = false;
	};

	class FakePromptSubmitter final : public PromptSubmitter {
	public:
		explicit FakePromptSubmitter(FakeContext *context)
		    : context_(context) {}

		void SubmitPrompt() override {
			context_->submission_thread = std::this_thread::get_id();
			const auto wait_for_release = [this] -> void {
				std::unique_lock<std::mutex> lock(context_->submission_mutex);
				context_->submission_ready = true;
				context_->submission_condition.notify_all();
				context_->submission_condition.wait(lock, [this] -> bool {
					return context_->release_submission;
				});
			};
			if (context_->coordinator_for_submission != nullptr) {
				context_->submission_started_before_password_return =
				    !howdy::pam::PromptCoordinatorTestAccess::PasswordCallReturned(
				        *context_->coordinator_for_submission);
			}
			++context_->prompt_submissions;
			if (context_->block_submission_after_start) {
				wait_for_release();
			}
			if (context_->fail_prompt_submission) {
				throw std::runtime_error("Failed to submit prompt");
			}
			if (context_->release_token_on_submission) {
				{
					std::unique_lock<std::mutex> lock(context_->token_mutex);
					context_->release_token = true;
				}
				context_->token_condition.notify_one();
			}
			context_->submission_finished = true;
			context_->submission_condition.notify_all();
		}

	private:
		FakeContext *context_;
	};

	class FakeNativePrompt final : public NativePrompt {
	public:
		explicit FakeNativePrompt(FakeContext *context)
		    : context_(context) {}

		[[nodiscard]] auto Available() const -> bool override {
			return context_->native_available;
		}

		[[nodiscard]] auto TerminalRestoreFailed() const noexcept -> bool override {
			return context_->native_terminal_restore_failed;
		}

		auto Install() -> int override {
			context_->native_install_thread = std::this_thread::get_id();
			if (context_->native_install_result == PAM_SUCCESS) {
				context_->native_prompt_installed = true;
			}
			return context_->native_install_result;
		}

		void RequestAbort() override {
			context_->native_abort_thread = std::this_thread::get_id();
			++context_->native_abort_calls;
			context_->native_prompt_completed = true;
			context_->native_prompt_condition.notify_one();
		}

		auto RestoreOriginal() noexcept -> howdy::pam::ConversationRestoreResult override {
			context_->native_restore_thread = std::this_thread::get_id();
			++context_->native_restore_calls;
			return context_->native_restore_result;
		}

	private:
		FakeContext *context_;
	};

	class FakeSecretPromptConversation final : public howdy::pam::SecretPromptConversation {
	public:
		explicit FakeSecretPromptConversation(FakeContext *context)
		    : context_(context) {}

		[[nodiscard]] auto Available() const -> bool override {
			return context_->secret_prompt_available;
		}

		auto Install() -> int override {
			return context_->secret_prompt_install_result;
		}

		auto RestoreOriginal() noexcept -> howdy::pam::ConversationRestoreResult override {
			++context_->secret_restore_calls;
			return context_->secret_restore_result;
		}

	private:
		FakeContext *context_;
	};

	struct CallbackCounts {
		int spawn     = 0;
		int wait      = 0;
		int terminate = 0;
		int cleanup   = 0;
		int preflight = 0;
		int submitter = 0;
		int auth      = 0;

		auto operator==(const CallbackCounts &) const -> bool = default;
	};

	inline auto SpawnCompareProcess(void *context, const howdy::pam::CompareLaunchRequest &request,
	                                pid_t *child_pid) -> int {
		auto &fake = *static_cast<FakeContext *>(context);

		++fake.spawn_calls;
		fake.spawned_config_path     = std::string(request.config_path);
		fake.spawned_username        = std::string(request.username);
		fake.spawned_user_models_dir = std::string(request.user_models_dir);
		fake.spawned_staged_runtime  = request.staged_runtime;

		if (fake.spawn_result != 0) {
			return fake.spawn_result;
		}

		*child_pid       = fake.next_child_pid;
		fake.spawned_pid = *child_pid;
		return 0;
	}

	inline auto WaitForNativePromptCompletion(FakeContext &fake) -> bool {
		std::unique_lock<std::mutex> lock(fake.native_prompt_mutex);
		if (!fake.native_prompt_condition.wait_for(lock, 2s, [&fake] -> bool {
			    return fake.native_prompt_seen.load();
		    })) {
			return false;
		}
		if (fake.complete_native_prompt &&
		    !fake.native_prompt_condition.wait_for(lock, 2s, [&fake] -> bool {
			    return fake.native_prompt_completed.load();
		    })) {
			return false;
		}
		if (fake.complete_native_prompt) {
			fake.pam_completion_observed_by_waiter = true;
		}
		return true;
	}

	inline void CancelAndReapCompare(void *context, pid_t child_pid) noexcept;

	inline auto WaitForCompare(void *context, pid_t child_pid,
	                           [[maybe_unused]] std::chrono::steady_clock::time_point deadline,
	                           void                                      *cancellation_context,
	                           howdy::pam::CompareCancellationRequestedFn cancellation_requested)
	    -> int {
		auto &fake = *static_cast<FakeContext *>(context);
		++fake.wait_calls;
		fake.waited_pid  = child_pid;
		fake.wait_thread = std::this_thread::get_id();
		if (fake.throw_compare_wait) {
			throw std::runtime_error("simulated compare wait failure");
		}
		if (fake.request_native_prompt) {
			if (!WaitForNativePromptCompletion(fake)) {
				CancelAndReapCompare(context, child_pid);
				return static_cast<int>(CompareExit::kAbort) << 8;
			}
		}
		while (true) {
			int         status = 0;
			const pid_t result = waitpid(child_pid, &status, WNOHANG);
			if (result == child_pid) {
				fake.last_wait_status = status;
				{
					std::scoped_lock lock(fake.reap_mutex);
					fake.child_reaped_by_wait = true;
				}
				fake.reap_condition.notify_one();
				while (fake.hold_reaped_until_cancel && cancellation_requested != nullptr &&
				       !cancellation_requested(cancellation_context)) {
					std::this_thread::yield();
				}
				return status;
			}
			if (result < 0 && errno == EINTR) {
				continue;
			}
			if (result == 0 && cancellation_requested != nullptr &&
			    cancellation_requested(cancellation_context)) {
				++fake.terminate_calls;
				fake.terminated_pid = child_pid;
				(void)kill(child_pid, SIGTERM);
				if (!howdy::test::process::ReapTestChild(child_pid, &status)) {
					return static_cast<int>(CompareExit::kAbort) << 8;
				}
				fake.last_wait_status = status;
				return status;
			}
			if (result == 0) {
				std::this_thread::sleep_for(1ms);
				continue;
			}
			return static_cast<int>(CompareExit::kAbort) << 8;
		}
	}

	inline void CancelAndReapCompare(void *context, pid_t child_pid) noexcept {
		auto &fake = *static_cast<FakeContext *>(context);
		++fake.cleanup_calls;
		fake.cleaned_pid = child_pid;
		(void)kill(child_pid, SIGTERM);
		int status = 0;
		(void)howdy::test::process::ReapTestChild(child_pid, &status);
		fake.last_wait_status = status;
	}

	inline auto InputPreflight(void *context) -> bool {
		auto &fake = *static_cast<FakeContext *>(context);
		++fake.preflight_calls;
		return fake.preflight_result;
	}

	inline auto CreatePromptSubmitter(void *context) -> std::unique_ptr<PromptSubmitter> {
		auto &fake = *static_cast<FakeContext *>(context);
		++fake.prompt_submitter_constructions;
		if (fake.fail_prompt_submitter_construction) {
			throw std::runtime_error("Failed to create prompt submitter");
		}
		if (fake.return_null_prompt_submitter) {
			return nullptr;
		}
		return std::make_unique<FakePromptSubmitter>(&fake);
	}

	inline auto CreateNativePrompt(void *context, pam_handle_t *pamh)
	    -> std::unique_ptr<NativePrompt> {
		(void)pamh;
		auto &fake                = *static_cast<FakeContext *>(context);
		fake.native_create_thread = std::this_thread::get_id();
		if (fake.throw_native_prompt) {
			throw std::runtime_error("Failed to create native prompt");
		}
		if (fake.throw_native_prompt_unknown) {
			throw 1;
		}
		return std::make_unique<FakeNativePrompt>(&fake);
	}

	inline auto CreateSecretPromptConversation(void *context, pam_handle_t *pamh,
	                                           howdy::pam::SecretPromptObserver observer)
	    -> std::unique_ptr<howdy::pam::SecretPromptConversation> {
		(void)pamh;
		auto &fake                  = *static_cast<FakeContext *>(context);
		fake.secret_prompt_observer = observer;
		if (fake.throw_secret_prompt) {
			throw std::runtime_error("Failed to create secret prompt observer");
		}
		if (fake.throw_secret_prompt_unknown) {
			throw 1;
		}
		if (fake.return_null_secret_prompt) {
			return nullptr;
		}
		return std::make_unique<FakeSecretPromptConversation>(&fake);
	}

	inline auto RequestTokenUntilRelease(FakeContext &fake) -> std::tuple<int, const char *> {
		std::unique_lock<std::mutex> lock(fake.token_mutex);
		fake.before_conversation = true;
		fake.token_condition.notify_all();
		if (!fake.token_condition.wait_for(lock, 2s, [&fake] -> bool {
			    return fake.release_token;
		    })) {
			fake.auth_token_active = false;
			return {PAM_SYSTEM_ERR, nullptr};
		}
		fake.auth_token_active = false;
		fake.token_returned    = true;
		fake.token_condition.notify_all();
		return {fake.token_result, nullptr};
	}

	inline auto RequestRealAuthToken(FakeContext &fake, pam_handle_t *pamh)
	    -> std::tuple<int, const char *> {
		if (fake.block_before_conversation) {
			std::unique_lock<std::mutex> lock(fake.token_mutex);
			fake.before_conversation = true;
			fake.token_condition.notify_all();
			if (!fake.token_condition.wait_for(lock, 2s, [&fake] -> bool {
				    return fake.release_conversation;
			    })) {
				fake.auth_token_active = false;
				return {PAM_SYSTEM_ERR, nullptr};
			}
		}
		if (fake.complete_without_conversation) {
			fake.auth_token_active = false;
			return {fake.token_result, nullptr};
		}
		const void *item   = nullptr;
		int         result = pam_get_item(pamh, PAM_CONV, &item);
		if (result == PAM_SUCCESS && item != nullptr) {
			const auto              *conversation = static_cast<const struct pam_conv *>(item);
			const struct pam_message message{
			    .msg_style = PAM_PROMPT_ECHO_OFF,
			    .msg       = "Password: ",
			};
			const struct pam_message *message_ptr = &message;
			struct pam_response      *response    = nullptr;
			result = conversation->conv(1, &message_ptr, &response, conversation->appdata_ptr);
			if (response != nullptr) {
				std::free(response->resp);
				std::free(response);
			}
		}
		fake.auth_token_active = false;
		return {result, nullptr};
	}

	inline auto RequestAuthToken(void *context, pam_handle_t *pamh)
	    -> std::tuple<int, const char *> {
		auto &fake = *static_cast<FakeContext *>(context);
		++fake.auth_token_calls;
		fake.auth_token_thread = std::this_thread::get_id();
		fake.auth_token_active = true;
		if (fake.throw_auth_token) {
			throw std::runtime_error("Failed to request auth token");
		}
		if (fake.throw_auth_token_unknown) {
			throw 1;
		}
		const auto generation =
		    !fake.suppress_secret_prompt && !fake.use_real_auth_token &&
		            fake.secret_prompt_observer.begin != nullptr
		        ? fake.secret_prompt_observer.begin(fake.secret_prompt_observer.context)
		        : 0;
		fake.active_prompt_generation = generation;

		struct GenerationScope {
			howdy::pam::SecretPromptObserver   observer{};
			howdy::pam::SecretPromptGeneration generation = 0;

			~GenerationScope() {
				if (generation != 0) {
					observer.end(observer.context, generation);
				}
			}
		} generation_scope{.observer = fake.secret_prompt_observer, .generation = generation};

		if (fake.block_token_until_release) {
			return RequestTokenUntilRelease(fake);
		}
		if (fake.use_real_auth_token) {
			return RequestRealAuthToken(fake, pamh);
		}
		if (fake.token_waits_for_reap) {
			std::unique_lock<std::mutex> lock(fake.reap_mutex);
			fake.reap_condition.wait_for(lock, 1s, [&fake] -> bool {
				return fake.child_reaped_by_wait;
			});
		}
		if (fake.request_native_prompt) {
			(void)pamh;
			fake.native_prompt_seen = true;
			fake.native_prompt_condition.notify_all();
			if (fake.complete_native_prompt) {
				fake.native_prompt_input_sent = true;
				fake.native_prompt_completed  = true;
				fake.native_prompt_condition.notify_one();
			} else {
				std::unique_lock<std::mutex> lock(fake.native_prompt_mutex);
				if (!fake.native_prompt_condition.wait_for(lock, 2s, [&fake] -> bool {
					    return fake.native_prompt_completed.load();
				    })) {
					fake.auth_token_active = false;
					return {PAM_SYSTEM_ERR, nullptr};
				}
			}
			if (fake.native_terminal_restore_failure_on_abort) {
				fake.native_terminal_restore_failed = true;
			}
			fake.auth_token_active = false;
			return {fake.token_result, nullptr};
		}
		std::this_thread::sleep_for(fake.token_delay);
		fake.auth_token_active = false;
		return {fake.token_result, nullptr};
	}

	inline auto Dependencies(FakeContext *context) -> PromptCoordinatorDependencies {
		return PromptCoordinatorDependencies{
		    .context                           = context,
		    .spawn_compare_process             = SpawnCompareProcess,
		    .wait_for_compare_process          = WaitForCompare,
		    .cancel_and_reap_compare_process   = CancelAndReapCompare,
		    .input_prompt_preflight            = InputPreflight,
		    .create_prompt_submitter           = CreatePromptSubmitter,
		    .create_native_prompt              = CreateNativePrompt,
		    .create_secret_prompt_conversation = CreateSecretPromptConversation,
		    .request_auth_token                = RequestAuthToken,
		};
	}

	inline auto WaitForSubmissionReady(FakeContext                        &context,
	                                   std::chrono::steady_clock::duration timeout) -> bool {
		std::unique_lock<std::mutex> lock(context.submission_mutex);
		return context.submission_condition.wait_for(lock, timeout, [&context] -> bool {
			return context.submission_ready.load();
		});
	}

	inline auto WaitForSubmissionFinished(FakeContext                        &context,
	                                      std::chrono::steady_clock::duration timeout) -> bool {
		std::unique_lock<std::mutex> lock(context.submission_mutex);
		return context.submission_condition.wait_for(lock, timeout, [&context] -> bool {
			return context.submission_finished.load();
		});
	}

	inline void ReleaseSubmission(FakeContext &context) {
		{
			std::scoped_lock lock(context.submission_mutex);
			context.release_submission = true;
		}
		context.submission_condition.notify_all();
	}

	inline auto GetCallbackCounts(const FakeContext &context) -> CallbackCounts {
		return CallbackCounts{
		    .spawn     = context.spawn_calls.load(),
		    .wait      = context.wait_calls.load(),
		    .terminate = context.terminate_calls.load(),
		    .cleanup   = context.cleanup_calls.load(),
		    .preflight = context.preflight_calls.load(),
		    .submitter = context.prompt_submitter_constructions.load(),
		    .auth      = context.auth_token_calls.load(),
		};
	}

	inline auto MakeCompareRequest(std::string_view config_path     = "/etc/howdy/config.ini",
	                               std::string_view username        = "alice",
	                               std::string_view user_models_dir = "/etc/howdy/models",
	                               bool             staged_runtime  = false)
	    -> howdy::pam::CompareLaunchRequest {
		return {
		    .config_path     = std::string(config_path),
		    .username        = std::string(username),
		    .user_models_dir = std::string(user_models_dir),
		    .staged_runtime  = staged_runtime,
		};
	}

	inline auto TimeoutWaitStatus() -> int {
		return static_cast<int>(CompareExit::kTimeoutReached) << 8;
	}
}  // namespace howdy::test::prompt_coordinator
