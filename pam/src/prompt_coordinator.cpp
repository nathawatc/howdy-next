#include "prompt_coordinator.hpp"

#include "common/compare_exit.hpp"
#ifdef HOWDY_PAM_TESTING
#	include "auth_flow_testing.hpp"
#	include "prompt_coordinator_testing.hpp"
#endif
#include "enter_device.hpp"
#include "prompt_workaround.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <future>
#include <stdexcept>
#include <syslog.h>
#include <unistd.h>

#include <security/pam_appl.h>
#include <security/pam_ext.h>

#include <sys/wait.h>

namespace {

	constexpr auto kPromptRetryDelay =
	    std::chrono::duration<int, std::chrono::milliseconds::period>(100);
	constexpr int kMaxPromptRetries = 5;

	using howdy::native::CompareExit;

	auto make_wait_exit_status(CompareExit exit_code) -> int {
		return static_cast<int>(exit_code) << 8;
	}

	struct PromptStopResult {
		bool enter_failed   = false;
		bool prompt_stopped = true;
	};

	void cleanup_native_prompt(optional_task<std::tuple<int, char *>> *pass_task,
	                           NativePromptConversation               *native_prompt) noexcept {
		if (pass_task == nullptr || native_prompt == nullptr) {
			return;
		}

		try {
			if (pass_task->active()) {
				native_prompt->request_abort();
			}
			pass_task->stop();
			native_prompt->restore_original();
		} catch (const std::exception &error) {
			syslog(LOG_CRIT, "Native prompt cleanup failed: %s", error.what());
		} catch (...) {
			syslog(LOG_CRIT, "Native prompt cleanup failed with non-standard exception");
		}
	}

	struct NativePromptCleanupGuard {
		optional_task<std::tuple<int, char *>> *pass_task     = nullptr;
		NativePromptConversation               *native_prompt = nullptr;

		~NativePromptCleanupGuard() {
			cleanup_native_prompt(pass_task, native_prompt);
		}
	};

	auto wait_for_compare_process(pid_t child_pid) -> int {
		while (true) {
			int         status = 0;
			const pid_t result = waitpid(child_pid, &status, 0);
			if (result == child_pid) {
				return status;
			}
			if (result < 0 && errno == EINTR) {
				continue;
			}

			syslog(LOG_ERR, "waitpid failed for compare process: %s (%d)", strerror(errno), errno);
			return make_wait_exit_status(CompareExit::kAbort);
		}
	}

	auto input_prompt_workaround_preflight() -> bool {
		if (euidaccess("/dev/uinput", W_OK | R_OK) != 0) {
			const int access_errno = errno;
			syslog(LOG_ERR, "Input prompt workaround unavailable: %s (%d)", strerror(access_errno),
			       access_errno);
			return false;
		}

		try {
			EnterDevice probe;
		} catch (const std::runtime_error &err) {
			syslog(LOG_ERR, "Input prompt workaround setup failed: %s", err.what());
			return false;
		}

		return true;
	}

	auto request_password_prompt_stop(optional_task<std::tuple<int, char *>> &pass_task,
	                                  const PromptStopPlan                   &plan,
	                                  NativePromptConversation *native_prompt) -> PromptStopResult {
		PromptStopResult result;
		if (!plan.stop_prompt) {
			return result;
		}

		if (plan.abort_prompt && native_prompt != nullptr) {
			native_prompt->request_abort();
		}

		if (plan.send_enter) {
			if (euidaccess("/dev/uinput", W_OK | R_OK) != 0) {
				syslog(LOG_WARNING, "Insufficient permissions to create the fake device");
				result.enter_failed = true;
			} else {
				try {
					EnterDevice enter_device;
					enter_device.send_enter_press();

					int retries = 0;
					for (; retries < kMaxPromptRetries &&
					       pass_task.wait(kPromptRetryDelay) == std::future_status::timeout;
					     retries++) {
						enter_device.send_enter_press();
					}

					if (retries == kMaxPromptRetries && pass_task.wait(std::chrono::milliseconds(
					                                        0)) == std::future_status::timeout) {
						syslog(LOG_WARNING, "Failed to send enter input before the retries limit");
						result.enter_failed = true;
					}
				} catch (const std::runtime_error &err) {
					syslog(LOG_WARNING, "Failed to send enter input: %s", err.what());
					result.enter_failed = true;
				}
			}
		}

		if (plan.send_enter &&
		    pass_task.wait(std::chrono::milliseconds(0)) == std::future_status::timeout) {
			result.prompt_stopped = false;
			return result;
		}

		pass_task.stop();
		return result;
	}

	auto wait_for_compare_process_dependency(void *context, pid_t child_pid) -> int {
		(void)context;
		return wait_for_compare_process(child_pid);
	}

	auto terminate_compare_process_dependency(void *context, pid_t child_pid) -> void {
		(void)context;
		if (kill(child_pid, SIGTERM) != 0 && errno != ESRCH) {
			syslog(LOG_WARNING, "Failed to terminate compare process: %s (%d)", strerror(errno),
			       errno);
		}
	}

	auto input_prompt_preflight_dependency(void *context) -> bool {
		(void)context;
		return input_prompt_workaround_preflight();
	}

	auto request_auth_token_dependency(void *context, pam_handle_t *pamh)
	    -> std::tuple<int, char *> {
		(void)context;
		char     *auth_tok_ptr = nullptr;
		const int auth_result =
		    pam_get_authtok(pamh, PAM_AUTHTOK, const_cast<const char **>(&auth_tok_ptr), nullptr);

		return {auth_result, auth_tok_ptr};
	}

}  // namespace

namespace howdy::pam {

	PromptCoordinator::PromptCoordinator(pam_handle_t *pamh, Workaround workaround,
	                                     bool ask_auth_tok, bool existing_auth_token,
	                                     PromptCoordinatorDependencies dependencies)
	    : pamh_(pamh)
	    , requested_workaround_(workaround)
	    , ask_auth_tok_(ask_auth_tok)
	    , existing_auth_token_(existing_auth_token)
	    , dependencies_(dependencies)
	    , effective_workaround_(workaround) {}

	PromptCoordinator::~PromptCoordinator() {
		NativePromptCleanupGuard cleanup{
		    .pass_task     = pass_task_ ? &*pass_task_ : nullptr,
		    .native_prompt = native_prompt_ ? &*native_prompt_ : nullptr,
		};
	}

	auto PromptCoordinator::valid() const -> bool {
		return dependencies_.wait_for_compare_process != nullptr &&
		       dependencies_.terminate_compare != nullptr &&
		       dependencies_.input_prompt_preflight != nullptr &&
		       dependencies_.request_auth_token != nullptr;
	}

	auto PromptCoordinator::run(pid_t compare_child_pid) -> PromptCoordinatorResult {
		if (run_started_) {
			return {.decision = PromptCoordinatorDecision::kAlreadyRun};
		}
		run_started_ = true;

		if (!valid()) {
			return {.decision = PromptCoordinatorDecision::kInvalidDependencies};
		}

		child_task_.emplace([this, compare_child_pid] {
			const int status =
			    dependencies_.wait_for_compare_process(dependencies_.context, compare_child_pid);

			{
				std::unique_lock<std::mutex> lock(mutex_);
				if (confirmation_type_ == ConfirmationType::Unset) {
					confirmation_type_ = ConfirmationType::Howdy;
				}
			}
			condition_.notify_one();
			return status;
		});
		child_task_->activate();

		if (requested_workaround_ == Workaround::Native && ask_auth_tok_ && !existing_auth_token_) {
			native_prompt_.emplace(pamh_);

			if (!native_prompt_->available()) {
				syslog(LOG_INFO,
				       "Native prompt conversation unavailable, falling back to input workaround");
				native_prompt_.reset();
				effective_workaround_ = Workaround::Input;
			} else {
				const int install_result = native_prompt_->install();
				if (install_result != PAM_SUCCESS) {
					syslog(LOG_WARNING, "Failed to install native prompt conversation: %d",
					       install_result);
					native_prompt_.reset();
					effective_workaround_ = Workaround::Input;
				}
			}
		}

		if (effective_workaround_ == Workaround::Input && ask_auth_tok_ && !existing_auth_token_ &&
		    !dependencies_.input_prompt_preflight(dependencies_.context)) {
			syslog(LOG_WARNING,
			       "Input prompt workaround preflight failed; falling back to standard PAM prompt");
			effective_workaround_ = Workaround::Off;
		}

		const bool ask_pass = effective_workaround_ == Workaround::Native
		                          ? native_prompt_.has_value() && !existing_auth_token_
		                          : should_ask_for_password(ask_auth_tok_, effective_workaround_,
		                                                    existing_auth_token_);

		pass_task_.emplace([this] {
			auto result = dependencies_.request_auth_token(dependencies_.context, pamh_);

			{
				std::unique_lock<std::mutex> lock(mutex_);
				if (confirmation_type_ == ConfirmationType::Unset) {
					confirmation_type_ = ConfirmationType::Pam;
				}
			}
			condition_.notify_one();
			return result;
		});

		if (ask_pass) {
			pass_task_->activate();
		}

		{
			std::unique_lock<std::mutex> lock(mutex_);
			condition_.wait(lock, [this] {
				return confirmation_type_ != ConfirmationType::Unset;
			});
		}

		if (confirmation_type_ == ConfirmationType::Pam) {
			dependencies_.terminate_compare(dependencies_.context, compare_child_pid);
			child_task_->stop();
			if (ask_pass) {
				pass_task_->stop();
				const auto [pam_result, password] = pass_task_->get();
				(void)password;
				return PromptCoordinatorResult{
				    .decision   = PromptCoordinatorDecision::kPamResult,
				    .pam_status = pam_result,
				};
			}
		}

		child_task_->stop();
		const int status = child_task_->get();

		if (WIFEXITED(status) && WEXITSTATUS(status) != EXIT_SUCCESS && ask_pass) {
			pass_task_->stop();
			const auto [pam_result, password] = pass_task_->get();
			(void)password;
			return PromptCoordinatorResult{
			    .decision       = PromptCoordinatorDecision::kPasswordFallback,
			    .compare_status = status,
			    .pam_status     = pam_result,
			};
		}

		const auto stop_plan =
		    plan_prompt_stop(ask_pass, ask_pass && pass_task_->ready(), effective_workaround_);
		const auto stop_result = request_password_prompt_stop(
		    *pass_task_, stop_plan, native_prompt_ ? &*native_prompt_ : nullptr);
		if (!stop_result.prompt_stopped) {
			syslog(LOG_ERR, "Input prompt workaround cancellation failed; waiting for "
			                "user/password prompt to complete");
			pass_task_->stop();
		}

		return PromptCoordinatorResult{
		    .decision       = PromptCoordinatorDecision::kHowdyResult,
		    .compare_status = status,
		    .enter_failed   = stop_result.enter_failed,
		    .prompt_stopped = stop_result.prompt_stopped,
		};
	}

	auto production_prompt_coordinator_dependencies() -> PromptCoordinatorDependencies {
		return PromptCoordinatorDependencies{
		    .wait_for_compare_process = wait_for_compare_process_dependency,
		    .terminate_compare        = terminate_compare_process_dependency,
		    .input_prompt_preflight   = input_prompt_preflight_dependency,
		    .request_auth_token       = request_auth_token_dependency,
		};
	}

}  // namespace howdy::pam

#ifdef HOWDY_PAM_TESTING
namespace howdy::pam::testing {

	void cleanup_native_prompt(optional_task<std::tuple<int, char *>> &pass_task,
	                           NativePromptConversation               &native_prompt) noexcept {
		::cleanup_native_prompt(&pass_task, &native_prompt);
	}

	auto request_password_prompt_stop(optional_task<std::tuple<int, char *>> &pass_task,
	                                  const PromptStopPlan &plan) -> PromptStopResult {
		const auto result = ::request_password_prompt_stop(pass_task, plan, nullptr);
		return PromptStopResult{.enter_failed   = result.enter_failed,
		                        .prompt_stopped = result.prompt_stopped};
	}

	auto wait_for_compare_process(pid_t child_pid) -> int {
		return ::wait_for_compare_process(child_pid);
	}

}  // namespace howdy::pam::testing
#endif
