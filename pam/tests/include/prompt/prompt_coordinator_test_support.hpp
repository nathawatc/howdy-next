#pragma once
#include "prompt/prompt_coordinator.hpp"
#include "prompt/workaround.hpp"
#include "protocol/compare_exit.hpp"
#include "runtime/compare_process.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <mutex>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <unistd.h>
#include <utility>
#include <vector>

#include <security/pam_appl.h>

#include <sys/wait.h>

namespace howdy::pam {
	class PromptCoordinatorTestAccess {
	public:
		static auto wait_for_compare_success(PromptCoordinator                  &coordinator,
		                                     std::chrono::steady_clock::duration timeout) -> bool {
			std::unique_lock<std::mutex> lock(coordinator.mutex_);
			return coordinator.condition_.wait_for(lock, timeout, [&coordinator] -> bool {
				return coordinator.state_.compare_succeeded;
			});
		}

		[[nodiscard]] static auto password_call_returned(PromptCoordinator &coordinator) -> bool {
			std::scoped_lock lock(coordinator.mutex_);
			return coordinator.state_.password_call_returned;
		}

		static auto wait_for_password_call_returned(PromptCoordinator                  &coordinator,
		                                            std::chrono::steady_clock::duration timeout)
		    -> bool {
			std::unique_lock<std::mutex> lock(coordinator.mutex_);
			return coordinator.condition_.wait_for(lock, timeout, [&coordinator] -> bool {
				return coordinator.state_.password_call_returned;
			});
		}

		static void request_shutdown(PromptCoordinator &coordinator) {
			{
				std::scoped_lock lock(coordinator.mutex_);
				coordinator.state_.shutdown_requested     = true;
				coordinator.state_.cancellation_requested = true;
				if (coordinator.state_.submission ==
				    PromptCoordinator::PromptSubmissionState::kClaimed) {
					coordinator.state_.submission =
					    PromptCoordinator::PromptSubmissionState::kPending;
					coordinator.state_.claimed_generation = 0;
				}
			}
			coordinator.condition_.notify_all();
		}

		static void prepare_claimed_submission(PromptCoordinator               &coordinator,
		                                       std::unique_ptr<PromptSubmitter> prompt_submitter) {
			std::scoped_lock lock(coordinator.mutex_);
			coordinator.prompt_submitter_            = std::move(prompt_submitter);
			coordinator.state_.first_completion      = PromptCoordinator::FirstCompletion::kCompare;
			coordinator.state_.compare_succeeded     = true;
			coordinator.state_.password_call_entered = true;
			coordinator.state_.secret_prompt_generation = 1;
			coordinator.state_.claimed_generation       = 1;
			coordinator.state_.secret_prompt_active     = true;
			coordinator.state_.submission = PromptCoordinator::PromptSubmissionState::kClaimed;
		}

		static auto prompt_submission_finished(PromptCoordinator &coordinator) -> bool {
			std::scoped_lock lock(coordinator.mutex_);
			return coordinator.state_.submission ==
			       PromptCoordinator::PromptSubmissionState::kFinished;
		}

		static void publish_password_call_returned(PromptCoordinator &coordinator) {
			coordinator.publish_password_call_returned();
		}

		static void close_prompt_generation(PromptCoordinator     &coordinator,
		                                    SecretPromptGeneration generation) {
			PromptCoordinator::secret_prompt_end(&coordinator, generation);
		}

		static auto begin_prompt_generation(PromptCoordinator &coordinator)
		    -> SecretPromptGeneration {
			return PromptCoordinator::secret_prompt_begin(&coordinator);
		}

		static void submit_prompt_for_generations(PromptCoordinator &coordinator) {
			coordinator.submit_prompt_for_generations();
		}
	};
}  // namespace howdy::pam

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

		void submit_prompt() override {
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
				    !howdy::pam::PromptCoordinatorTestAccess::password_call_returned(
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

		[[nodiscard]] auto available() const -> bool override {
			return context_->native_available;
		}

		[[nodiscard]] auto terminal_restore_failed() const noexcept -> bool override {
			return context_->native_terminal_restore_failed;
		}

		auto install() -> int override {
			context_->native_install_thread = std::this_thread::get_id();
			if (context_->native_install_result == PAM_SUCCESS) {
				context_->native_prompt_installed = true;
			}
			return context_->native_install_result;
		}

		void request_abort() override {
			context_->native_abort_thread = std::this_thread::get_id();
			++context_->native_abort_calls;
			context_->native_prompt_completed = true;
			context_->native_prompt_condition.notify_one();
		}

		auto restore_original() noexcept -> howdy::pam::ConversationRestoreResult override {
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

		[[nodiscard]] auto available() const -> bool override {
			return context_->secret_prompt_available;
		}

		auto install() -> int override {
			return context_->secret_prompt_install_result;
		}

		auto restore_original() noexcept -> howdy::pam::ConversationRestoreResult override {
			++context_->secret_restore_calls;
			return context_->secret_restore_result;
		}

	private:
		FakeContext *context_;
	};

	class ScopedFd {
	public:
		ScopedFd() = default;

		explicit ScopedFd(int fd)
		    : fd_(fd) {}

		ScopedFd(const ScopedFd &)                     = delete;
		auto operator=(const ScopedFd &) -> ScopedFd & = delete;

		~ScopedFd() {
			reset();
		}

		[[nodiscard]] auto get() const -> int {
			return fd_;
		}

		void reset(int fd = -1) {
			if (fd_ >= 0) {
				close(fd_);
			}
			fd_ = fd;
		}

	private:
		int fd_ = -1;
	};

	struct PosixSpawnCapture {
		int                               init_calls          = 0;
		int                               addclosefrom_calls  = 0;
		int                               destroy_calls       = 0;
		int                               spawn_calls         = 0;
		int                               init_result         = 0;
		int                               addclosefrom_result = 0;
		int                               spawn_result        = 0;
		int                               closefrom_fd        = -1;
		pid_t                             next_pid            = 4242;
		posix_spawn_file_actions_t       *initialized_actions = nullptr;
		posix_spawn_file_actions_t       *closefrom_actions   = nullptr;
		posix_spawn_file_actions_t       *destroyed_actions   = nullptr;
		const posix_spawn_file_actions_t *spawn_actions       = nullptr;
		std::string                       path;
		std::vector<std::string>          argv;
		std::vector<std::string>          environment;
	};

	inline auto capture_posix_spawn_file_actions_init(void                       *context,
	                                                  posix_spawn_file_actions_t *actions) -> int {
		auto &capture = *static_cast<PosixSpawnCapture *>(context);
		++capture.init_calls;
		capture.initialized_actions = actions;
		return capture.init_result;
	}

	inline auto capture_posix_spawn_file_actions_addclosefrom(void                       *context,
	                                                          posix_spawn_file_actions_t *actions,
	                                                          int from_fd) -> int {
		auto &capture = *static_cast<PosixSpawnCapture *>(context);
		++capture.addclosefrom_calls;
		capture.closefrom_actions = actions;
		capture.closefrom_fd      = from_fd;
		return capture.addclosefrom_result;
	}

	inline auto capture_posix_spawn_file_actions_destroy(void                       *context,
	                                                     posix_spawn_file_actions_t *actions)
	    -> int {
		auto &capture = *static_cast<PosixSpawnCapture *>(context);
		++capture.destroy_calls;
		capture.destroyed_actions = actions;
		return 0;
	}

	inline auto capture_posix_spawn(const howdy::pam::compare_process::SpawnRequest &request)
	    -> int {
		auto &capture = *static_cast<PosixSpawnCapture *>(request.context);
		++capture.spawn_calls;
		capture.spawn_actions = request.actions;
		capture.path          = request.path;
		for (char *const *argument = request.argv; *argument != nullptr; ++argument) {
			capture.argv.emplace_back(*argument);
		}
		for (char *const *entry = request.envp; *entry != nullptr; ++entry) {
			capture.environment.emplace_back(*entry);
		}

		if (capture.spawn_result == 0) {
			*request.child_pid = capture.next_pid;
		}
		return capture.spawn_result;
	}

	inline auto posix_spawn_operations(void *context) -> howdy::pam::compare_process::Operations {
		return {
		    .context                   = context,
		    .file_actions_init         = capture_posix_spawn_file_actions_init,
		    .file_actions_addclosefrom = capture_posix_spawn_file_actions_addclosefrom,
		    .file_actions_destroy      = capture_posix_spawn_file_actions_destroy,
		    .spawn                     = capture_posix_spawn,
		};
	}

	inline auto original_conversation(int num_msg, const struct pam_message **messages,
	                                  struct pam_response **response, void *appdata_ptr) -> int {
		(void)num_msg;
		(void)messages;
		if (response != nullptr) {
			*response = nullptr;
		}
		if (appdata_ptr != nullptr) {
			++static_cast<FakeContext *>(appdata_ptr)->original_conversation_calls;
		}
		return PAM_CONV_ERR;
	}

	class NativePamFixture {
	public:
		explicit NativePamFixture(FakeContext *context)
		    : context_(context)
		    , original_conv_{.conv = original_conversation, .appdata_ptr = context} {}

		NativePamFixture(const NativePamFixture &)                     = delete;
		auto operator=(const NativePamFixture &) -> NativePamFixture & = delete;

		~NativePamFixture() {
			if (pamh_ != nullptr) {
				pam_end(pamh_, PAM_SUCCESS);
			}
		}

		auto start(bool with_tty) -> bool {
			if (pam_start("howdy-prompt-coordinator-test", "test-user", &original_conv_, &pamh_) !=
			    PAM_SUCCESS) {
				return false;
			}
			if (!with_tty) {
				return true;
			}

			master_fd_.reset(posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC));
			if (master_fd_.get() < 0 || grantpt(master_fd_.get()) != 0 ||
			    unlockpt(master_fd_.get()) != 0) {
				return false;
			}
			char *slave_path = ptsname(master_fd_.get());
			if (slave_path == nullptr || pam_set_item(pamh_, PAM_TTY, slave_path) != PAM_SUCCESS) {
				return false;
			}
			context_->prompt_master_fd = master_fd_.get();
			return true;
		}

		[[nodiscard]] auto pamh() const -> pam_handle_t * {
			return pamh_;
		}

		[[nodiscard]] auto original_conversation_restored() const -> bool {
			const void *item = nullptr;
			if (pam_get_item(pamh_, PAM_CONV, &item) != PAM_SUCCESS || item == nullptr) {
				return false;
			}
			const auto *conversation = static_cast<const struct pam_conv *>(item);
			return conversation->conv == original_conv_.conv &&
			       conversation->appdata_ptr == original_conv_.appdata_ptr;
		}

	private:
		FakeContext    *context_ = nullptr;
		struct pam_conv original_conv_{};
		pam_handle_t   *pamh_ = nullptr;
		ScopedFd        master_fd_;
	};

	struct CallbackCounts {
		int spawn     = 0;
		int wait      = 0;
		int terminate = 0;
		int preflight = 0;
		int submitter = 0;
		int auth      = 0;

		auto operator==(const CallbackCounts &) const -> bool = default;
	};

	inline auto spawn_compare_process(void                                   *context,
	                                  const howdy::pam::CompareLaunchRequest &request,
	                                  pid_t                                  *child_pid) -> int {
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

	inline auto wait_for_native_prompt_completion(FakeContext &fake) -> bool {
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

	inline auto reap_test_child(pid_t child_pid, int *status) -> bool {
		pid_t waited;
		do {
			waited = waitpid(child_pid, status, 0);
		} while (waited < 0 && errno == EINTR);
		if (waited == child_pid) {
			return true;
		}
		std::cerr << "waitpid(" << child_pid << ") failed: errno=" << errno << '\n';
		return false;
	}

	inline auto wait_for_compare(void *context, pid_t child_pid,
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
			if (!wait_for_native_prompt_completion(fake)) {
				howdy::pam::compare_process::cancel_and_reap(child_pid);
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
				if (!reap_test_child(child_pid, &status)) {
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

	inline auto watchdog_wait_for_compare(
	    void *context, pid_t child_pid, std::chrono::steady_clock::time_point deadline,
	    void                                      *cancellation_context,
	    howdy::pam::CompareCancellationRequestedFn cancellation_requested) -> int {
		auto &fake = *static_cast<FakeContext *>(context);
		++fake.wait_calls;
		fake.waited_pid      = child_pid;
		const auto remaining = std::max(deadline - std::chrono::steady_clock::now(),
		                                std::chrono::steady_clock::duration::zero());
		const int  status    = howdy::pam::compare_process::wait_until(
		    child_pid, std::chrono::steady_clock::now() + remaining, cancellation_context,
		    cancellation_requested);
		if (cancellation_requested != nullptr && cancellation_requested(cancellation_context)) {
			++fake.terminate_calls;
			fake.terminated_pid = child_pid;
		}
		fake.last_wait_status = status;
		return status;
	}

	inline auto input_preflight(void *context) -> bool {
		auto &fake = *static_cast<FakeContext *>(context);
		++fake.preflight_calls;
		return fake.preflight_result;
	}

	inline auto create_prompt_submitter(void *context) -> std::unique_ptr<PromptSubmitter> {
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

	inline auto create_native_prompt(void *context, pam_handle_t *pamh)
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

	inline auto create_secret_prompt_conversation(void *context, pam_handle_t *pamh,
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

	inline auto request_token_until_release(FakeContext &fake) -> std::tuple<int, const char *> {
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

	inline auto request_real_auth_token(FakeContext &fake, pam_handle_t *pamh)
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

	inline auto request_auth_token(void *context, pam_handle_t *pamh)
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
			return request_token_until_release(fake);
		}
		if (fake.use_real_auth_token) {
			return request_real_auth_token(fake, pamh);
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

	inline auto dependencies(FakeContext *context) -> PromptCoordinatorDependencies {
		return PromptCoordinatorDependencies{
		    .context                           = context,
		    .spawn_compare_process             = spawn_compare_process,
		    .wait_for_compare_process          = wait_for_compare,
		    .input_prompt_preflight            = input_preflight,
		    .create_prompt_submitter           = create_prompt_submitter,
		    .create_native_prompt              = create_native_prompt,
		    .create_secret_prompt_conversation = create_secret_prompt_conversation,
		    .request_auth_token                = request_auth_token,
		};
	}

	inline auto wait_for_submission_ready(FakeContext                        &context,
	                                      std::chrono::steady_clock::duration timeout) -> bool {
		std::unique_lock<std::mutex> lock(context.submission_mutex);
		return context.submission_condition.wait_for(lock, timeout, [&context] -> bool {
			return context.submission_ready.load();
		});
	}

	inline auto wait_for_submission_finished(FakeContext                        &context,
	                                         std::chrono::steady_clock::duration timeout) -> bool {
		std::unique_lock<std::mutex> lock(context.submission_mutex);
		return context.submission_condition.wait_for(lock, timeout, [&context] -> bool {
			return context.submission_finished.load();
		});
	}

	inline void release_submission(FakeContext &context) {
		{
			std::scoped_lock lock(context.submission_mutex);
			context.release_submission = true;
		}
		context.submission_condition.notify_all();
	}

	inline auto callback_counts(const FakeContext &context) -> CallbackCounts {
		return CallbackCounts{
		    .spawn     = context.spawn_calls.load(),
		    .wait      = context.wait_calls.load(),
		    .terminate = context.terminate_calls.load(),
		    .preflight = context.preflight_calls.load(),
		    .submitter = context.prompt_submitter_constructions.load(),
		    .auth      = context.auth_token_calls.load(),
		};
	}

	inline auto make_compare_request(std::string_view config_path     = "/etc/howdy/config.ini",
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

	inline auto spawn_child(int exit_code, std::chrono::milliseconds delay = {}) -> pid_t {
		const pid_t child_pid = fork();
		if (child_pid == 0) {
			std::this_thread::sleep_for(delay);
			_exit(exit_code);
		}
		return child_pid;
	}

	inline auto spawn_signaled_child(int signal_number) -> pid_t {
		const pid_t child_pid = fork();
		if (child_pid == 0) {
			raise(signal_number);
			_exit(EXIT_FAILURE);
		}
		return child_pid;
	}

	inline auto spawn_blocked_child() -> pid_t {
		const pid_t child_pid = fork();
		if (child_pid == 0) {
			while (true) {
				pause();
			}
		}
		return child_pid;
	}

	inline void ignore_sigterm([[maybe_unused]] int signal_number) {}

	inline auto spawn_sigterm_ignoring_child() -> pid_t {
		std::array<int, 2> ready_pipe = {-1, -1};
		if (pipe(ready_pipe.data()) != 0) {
			return -1;
		}

		const pid_t child_pid = fork();
		if (child_pid == 0) {
			close(ready_pipe[0]);
			struct sigaction action = {};
			action.sa_handler       = ignore_sigterm;
			sigemptyset(&action.sa_mask);
			if (sigaction(SIGTERM, &action, nullptr) != 0) {
				_exit(EXIT_FAILURE);
			}
			const char ready = '1';
			ssize_t    write_result;
			do {
				write_result = write(ready_pipe[1], &ready, sizeof(ready));
			} while (write_result < 0 && errno == EINTR);
			if (std::cmp_not_equal(write_result, sizeof(ready))) {
				_exit(EXIT_FAILURE);
			}
			while (true) {
				pause();
			}
		}

		close(ready_pipe[1]);
		char ready = '\0';
		while (read(ready_pipe[0], &ready, 1) < 0 && errno == EINTR) {
		}
		close(ready_pipe[0]);
		if (child_pid <= 0 || ready != '1') {
			if (child_pid > 0) {
				(void)kill(child_pid, SIGKILL);
				(void)reap_test_child(child_pid, nullptr);
			}
			return -1;
		}
		return child_pid;
	}

	inline auto child_reaped(pid_t child_pid) -> bool {
		errno                   = 0;
		const pid_t wait_result = waitpid(child_pid, nullptr, WNOHANG);
		return wait_result == -1 && errno == ECHILD;
	}

	inline auto timeout_wait_status() -> int {
		return static_cast<int>(CompareExit::kTimeoutReached) << 8;
	}

}  // namespace howdy::test::prompt_coordinator
