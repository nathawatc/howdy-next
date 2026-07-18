#ifndef _GNU_SOURCE
#	define _GNU_SOURCE
#endif

#include "prompt/prompt_coordinator.hpp"

#include "module/prompt_workaround.hpp"
#include "prompt/enter_device.hpp"
#include "runtime/compare_process.hpp"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <future>
#include <spawn.h>
#include <stdexcept>
#include <syslog.h>
#include <unistd.h>

#include <security/pam_appl.h>
#include <security/pam_ext.h>

#include <sys/wait.h>

namespace {

	constexpr auto kPromptCompletionGrace = std::chrono::milliseconds(100);

	auto input_workaround_access() -> int {
		return euidaccess("/dev/uinput", W_OK | R_OK);
	}

	auto input_prompt_workaround_preflight() -> bool {
		if (input_workaround_access() != 0) {
			const int access_errno = errno;
			syslog(LOG_ERR, "Input prompt workaround unavailable: %s (%d)", strerror(access_errno),
			       access_errno);
			return false;
		}

		return true;
	}

	auto input_prompt_preflight_dependency(void *context) -> bool {
		(void)context;
		return input_prompt_workaround_preflight();
	}

	auto request_auth_token_dependency(void *context, pam_handle_t *pamh)
	    -> std::tuple<int, const char *> {
		(void)context;
		const char *auth_tok_ptr = nullptr;
		const int   auth_result  = pam_get_authtok(pamh, PAM_AUTHTOK, &auth_tok_ptr, nullptr);

		return {auth_result, auth_tok_ptr};
	}

	auto create_enter_device_dependency(void *context) -> std::unique_ptr<EnterDevice> {
		(void)context;
		return create_enter_device();
	}

	auto create_native_prompt_dependency(void *context, pam_handle_t *pamh)
	    -> std::unique_ptr<NativePrompt> {
		(void)context;
		return std::make_unique<NativePromptConversation>(pamh);
	}

}  // namespace

namespace howdy::pam {

	void cleanup_native_prompt(optional_task<std::tuple<int, const char *>> *pass_task,
	                           NativePrompt *native_prompt) noexcept {
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

	auto request_password_prompt_stop(optional_task<std::tuple<int, const char *>> &pass_task,
	                                  const PromptStopPlan &plan, NativePrompt *native_prompt,
	                                  EnterDevice *enter_device) -> PromptStopResult {
		PromptStopResult result;
		if (!plan.stop_prompt) {
			return result;
		}

		if (plan.abort_prompt && native_prompt != nullptr) {
			native_prompt->request_abort();
		}

		if (plan.send_enter && pass_task.ready()) {
			pass_task.stop();
			return result;
		}

		if (plan.send_enter) {
			try {
				if (enter_device == nullptr) {
					throw std::runtime_error("Input prompt workaround device unavailable");
				}
				enter_device->send_enter_press();
				if (pass_task.wait(kPromptCompletionGrace) == std::future_status::timeout) {
					result.enter_failed   = true;
					result.prompt_stopped = false;
					return result;
				}
			} catch (const std::runtime_error &err) {
				syslog(LOG_WARNING, "Failed to send enter input: %s", err.what());
				result.enter_failed = true;
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

	namespace {
		struct NativePromptCleanupGuard {
			optional_task<std::tuple<int, const char *>> *pass_task     = nullptr;
			NativePrompt                                 *native_prompt = nullptr;

			~NativePromptCleanupGuard() {
				cleanup_native_prompt(pass_task, native_prompt);
			}
		};
	}  // namespace

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

	PromptCoordinator::~PromptCoordinator() {
		NativePromptCleanupGuard cleanup{
		    .pass_task     = pass_task_ ? &*pass_task_ : nullptr,
		    .native_prompt = native_prompt_.get(),
		};
	}

	auto PromptCoordinator::valid() const -> bool {
		return dependencies_.spawn_compare_process != nullptr &&
		       dependencies_.wait_for_compare_process != nullptr &&
		       dependencies_.terminate_compare != nullptr &&
		       dependencies_.input_prompt_preflight != nullptr &&
		       dependencies_.create_enter_device != nullptr &&
		       dependencies_.create_native_prompt != nullptr &&
		       dependencies_.request_auth_token != nullptr &&
		       hard_timeout_ > std::chrono::steady_clock::duration::zero();
	}

	auto
	PromptCoordinator::start_compare_task(pid_t                                 child_pid,
	                                      std::chrono::steady_clock::time_point compare_deadline)
	    -> optional_task<int> & {
		auto &task = child_task_.emplace([this, child_pid, compare_deadline] -> int {
			const int status = dependencies_.wait_for_compare_process(dependencies_.context,
			                                                          child_pid, compare_deadline);
			{
				std::unique_lock<std::mutex> lock(mutex_);
				if (confirmation_type_ == ConfirmationType::Unset) {
					confirmation_type_ = ConfirmationType::Howdy;
				}
			}
			condition_.notify_one();
			return status;
		});
		task.activate();
		return task;
	}

	auto PromptCoordinator::configure_prompt_workaround() -> bool {
		const bool wants_native_prompt = requested_workaround_ == Workaround::Native ||
		                                 requested_workaround_ == Workaround::NativeInput;
		if (wants_native_prompt && ask_auth_tok_ && !existing_auth_token_) {
			native_prompt_ = dependencies_.create_native_prompt(dependencies_.context, pamh_);
			if (native_prompt_ == nullptr || !native_prompt_->available()) {
				const bool fallback_to_input = requested_workaround_ == Workaround::NativeInput;
				syslog(LOG_INFO,
				       fallback_to_input
				           ? "Native prompt conversation unavailable, falling back to input "
				             "workaround"
				           : "Native prompt conversation unavailable, disabling prompt "
				             "workaround");
				effective_workaround_ = fallback_to_input ? Workaround::Input : Workaround::Off;
				native_prompt_.reset();
			} else {
				const int install_result = native_prompt_->install();
				if (install_result == PAM_SUCCESS) {
					effective_workaround_ = Workaround::Native;
				} else {
					syslog(LOG_WARNING, "Failed to install native prompt conversation: %d",
					       install_result);
					effective_workaround_ = requested_workaround_ == Workaround::NativeInput
					                            ? Workaround::Input
					                            : Workaround::Off;
					native_prompt_.reset();
				}
			}
		}

		configure_input_workaround();
		return effective_workaround_ == Workaround::Native
		           ? native_prompt_ != nullptr && !existing_auth_token_
		           : should_ask_for_password(ask_auth_tok_, effective_workaround_,
		                                     existing_auth_token_);
	}

	void PromptCoordinator::configure_input_workaround() {
		if (effective_workaround_ != Workaround::Input || !ask_auth_tok_ || existing_auth_token_) {
			return;
		}

		if (!dependencies_.input_prompt_preflight(dependencies_.context)) {
			syslog(LOG_WARNING, "Input prompt workaround preflight failed; falling back to "
			                    "standard PAM prompt");
			effective_workaround_ = Workaround::Off;
			return;
		}

		try {
			enter_device_ = dependencies_.create_enter_device(dependencies_.context);
			if (enter_device_ == nullptr) {
				syslog(LOG_ERR, "Input prompt workaround setup failed: device unavailable");
				effective_workaround_ = Workaround::Off;
			}
		} catch (const std::exception &err) {
			syslog(LOG_ERR, "Input prompt workaround setup failed: %s", err.what());
			effective_workaround_ = Workaround::Off;
		} catch (...) {
			syslog(LOG_ERR, "Input prompt workaround setup failed with non-standard exception");
			effective_workaround_ = Workaround::Off;
		}
	}

	auto PromptCoordinator::start_password_task(bool ask_pass)
	    -> optional_task<std::tuple<int, const char *>> & {
		auto &task = pass_task_.emplace([this] -> std::tuple<int, const char *> {
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
			task.activate();
		}
		return task;
	}

	auto PromptCoordinator::run(const CompareLaunchRequest  &request,
	                            const std::function<void()> &report_input_failure)
	    -> PromptCoordinatorResult {
		if (run_started_) {
			return {.decision = PromptCoordinatorDecision::kAlreadyRun};
		}
		run_started_ = true;

		if (!valid()) {
			return {.decision = PromptCoordinatorDecision::kInvalidDependencies};
		}

		const auto compare_deadline = std::chrono::steady_clock::now() + hard_timeout_;
		pid_t      child_pid        = -1;
		const int  spawn_result =
		    dependencies_.spawn_compare_process(dependencies_.context, request, &child_pid);

		if (spawn_result != 0 || child_pid <= 0) {
			if (spawn_result != 0) {
				syslog(LOG_ERR, "Can't spawn the howdy process: %s (%d)", strerror(spawn_result),
				       spawn_result);
			} else {
				syslog(LOG_ERR, "Can't spawn the howdy process: invalid child pid");
			}
			return {
			    .decision = PromptCoordinatorDecision::kCompareSpawnFailed,
			};
		}

		auto      &child_task = start_compare_task(child_pid, compare_deadline);
		const bool ask_pass   = configure_prompt_workaround();
		auto      &pass_task  = start_password_task(ask_pass);

		{
			std::unique_lock<std::mutex> lock(mutex_);
			condition_.wait(lock, [this] -> bool {
				return confirmation_type_ != ConfirmationType::Unset;
			});
		}

		if (confirmation_type_ == ConfirmationType::Pam) {
			dependencies_.terminate_compare(dependencies_.context, child_pid);
			child_task.stop();
			if (ask_pass) {
				pass_task.stop();
				const auto [pam_result, password] = pass_task.get();
				(void)password;
				return PromptCoordinatorResult{
				    .decision   = PromptCoordinatorDecision::kPamResult,
				    .pam_status = pam_result,
				};
			}
		}

		child_task.stop();
		const int status = child_task.get();

		const bool compare_succeeded = WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS;
		if (!compare_succeeded && ask_pass) {
			pass_task.stop();
			const auto [pam_result, password] = pass_task.get();
			(void)password;
			return PromptCoordinatorResult{
			    .decision       = PromptCoordinatorDecision::kPasswordFallback,
			    .compare_status = status,
			    .pam_status     = pam_result,
			};
		}

		const auto stop_plan =
		    plan_prompt_stop(ask_pass, ask_pass && pass_task.ready(), effective_workaround_);
		auto      *native_prompt = native_prompt_.get();
		auto      *enter_device  = enter_device_.get();
		const auto stop_result =
		    request_password_prompt_stop(pass_task, stop_plan, native_prompt, enter_device);
		if (stop_result.enter_failed && report_input_failure) {
			try {
				report_input_failure();
			} catch (const std::exception &error) {
				syslog(LOG_WARNING, "Input prompt failure callback failed: %s", error.what());
			} catch (...) {
				syslog(LOG_WARNING,
				       "Input prompt failure callback failed with non-standard exception");
			}
		}
		if (!stop_result.prompt_stopped) {
			syslog(LOG_ERR, "Input prompt workaround cancellation failed; waiting for "
			                "user/password prompt to complete");
			pass_task.stop();
		}

		return PromptCoordinatorResult{
		    .decision       = PromptCoordinatorDecision::kHowdyResult,
		    .compare_status = status,
		    .prompt_stopped = stop_result.prompt_stopped,
		};
	}

	auto production_prompt_coordinator_dependencies() -> PromptCoordinatorDependencies {
		return PromptCoordinatorDependencies{
		    .spawn_compare_process    = compare_process::spawn,
		    .wait_for_compare_process = compare_process::wait,
		    .terminate_compare        = compare_process::terminate,
		    .input_prompt_preflight   = input_prompt_preflight_dependency,
		    .create_enter_device      = create_enter_device_dependency,
		    .create_native_prompt     = create_native_prompt_dependency,
		    .request_auth_token       = request_auth_token_dependency,
		};
	}

}  // namespace howdy::pam
