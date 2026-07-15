#ifndef _GNU_SOURCE
#	define _GNU_SOURCE
#endif

#include "prompt_coordinator.hpp"

#include "common/compare_exit.hpp"
#ifdef HOWDY_PAM_TESTING
#	include "auth_flow_testing.hpp"
#	include "prompt_coordinator_testing.hpp"
#endif
#include "enter_device.hpp"
#include "paths.hpp"
#include "prompt_workaround.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <future>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <syslog.h>
#include <thread>
#include <unistd.h>

#include <security/pam_appl.h>
#include <security/pam_ext.h>

#include <sys/wait.h>

namespace {
	using PosixSpawnFileActionsInitFn = int (*)(void *context, posix_spawn_file_actions_t *actions);
	using PosixSpawnFileActionsAddCloseFromFn = int (*)(void                       *context,
	                                                    posix_spawn_file_actions_t *actions,
	                                                    int                         from_fd);
	using PosixSpawnFileActionsDestroyFn      = int (*)(void                       *context,
	                                                    posix_spawn_file_actions_t *actions);

#ifdef HOWDY_PAM_TESTING
	using PosixSpawnRequest = howdy::pam::testing::PosixSpawnRequest;
#else
	struct PosixSpawnRequest {
		void                             *context;
		pid_t                            *child_pid;
		const char                       *path;
		const posix_spawn_file_actions_t *actions;
		char *const                      *argv;
		char *const                      *envp;
	};
#endif

	using PosixSpawnFn = int (*)(const PosixSpawnRequest &request);

	struct PosixSpawnOperations {
		PosixSpawnFileActionsInitFn         file_actions_init;
		PosixSpawnFileActionsAddCloseFromFn file_actions_addclosefrom;
		PosixSpawnFileActionsDestroyFn      file_actions_destroy;
		PosixSpawnFn                        spawn;
	};

	constexpr auto kPromptRetryDelay =
	    std::chrono::duration<int, std::chrono::milliseconds::period>(100);
	constexpr int  kMaxPromptRetries        = 5;
	constexpr auto kCompareWaitPollInterval = std::chrono::milliseconds(10);
	// Lets compare process perform SIGTERM cleanup without extending scan deadline.
	constexpr auto kCompareTerminationGrace = std::chrono::milliseconds(250);

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

	auto try_wait_for_compare(pid_t child_pid, int *status) -> bool {
		while (true) {
			const pid_t result = waitpid(child_pid, status, WNOHANG);
			if (result == child_pid) {
				return true;
			}
			if (result == 0) {
				return false;
			}
			if (errno == EINTR) {
				continue;
			}
			syslog(LOG_ERR, "waitpid failed for compare process: %s (%d)", strerror(errno), errno);
			*status = make_wait_exit_status(CompareExit::kAbort);
			return true;
		}
	}

	auto wait_for_compare_until(pid_t child_pid, std::chrono::steady_clock::time_point deadline)
	    -> std::optional<int> {
		using Clock = std::chrono::steady_clock;
		while (true) {
			int status = 0;
			if (try_wait_for_compare(child_pid, &status)) {
				return status;
			}
			const auto now = Clock::now();
			if (now >= deadline) {
				return std::nullopt;
			}
			std::this_thread::sleep_for(
			    std::min(std::chrono::duration_cast<Clock::duration>(kCompareWaitPollInterval),
			             deadline - now));
		}
	}

	auto wait_for_compare_process(pid_t child_pid, std::chrono::steady_clock::time_point deadline)
	    -> int {
		using Clock = std::chrono::steady_clock;

		if (const auto status = wait_for_compare_until(child_pid, deadline); status.has_value()) {
			return *status;
		}

		int status = 0;
		if (try_wait_for_compare(child_pid, &status)) {
			return status;
		}
		if (kill(child_pid, SIGTERM) != 0 && errno != ESRCH) {
			syslog(LOG_WARNING, "Failed to terminate timed-out compare process: %s (%d)",
			       strerror(errno), errno);
		}

		if (wait_for_compare_until(child_pid, Clock::now() + kCompareTerminationGrace)
		        .has_value()) {
			return make_wait_exit_status(CompareExit::kTimeoutReached);
		}
		if (kill(child_pid, SIGKILL) != 0 && errno != ESRCH) {
			syslog(LOG_WARNING, "Failed to kill timed-out compare process: %s (%d)",
			       strerror(errno), errno);
		}

		while (true) {
			status             = 0;
			const pid_t result = waitpid(child_pid, &status, 0);
			if (result == child_pid) {
				return make_wait_exit_status(CompareExit::kTimeoutReached);
			}
			if (result < 0 && errno == EINTR) {
				continue;
			}
			if (result < 0 && errno != ECHILD) {
				syslog(LOG_ERR, "waitpid failed while reaping timed-out compare process: %s (%d)",
				       strerror(errno), errno);
			}
			return make_wait_exit_status(CompareExit::kTimeoutReached);
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

	auto wait_for_compare_process_dependency(void *context, pid_t child_pid,
	                                         std::chrono::steady_clock::time_point deadline)
	    -> int {
		(void)context;
		return wait_for_compare_process(child_pid, deadline);
	}

	auto call_posix_spawn_file_actions_init(void *context, posix_spawn_file_actions_t *actions)
	    -> int {
		(void)context;
		return posix_spawn_file_actions_init(actions);
	}

	auto call_posix_spawn_file_actions_addclosefrom(void                       *context,
	                                                posix_spawn_file_actions_t *actions,
	                                                int                         from_fd) -> int {
		(void)context;
		return posix_spawn_file_actions_addclosefrom_np(actions, from_fd);
	}

	auto call_posix_spawn_file_actions_destroy(void *context, posix_spawn_file_actions_t *actions)
	    -> int {
		(void)context;
		return posix_spawn_file_actions_destroy(actions);
	}

	auto call_posix_spawn(const PosixSpawnRequest &request) -> int {
		(void)request.context;
		return posix_spawn(request.child_pid, request.path, request.actions, nullptr, request.argv,
		                   request.envp);
	}

	constexpr PosixSpawnOperations kPosixSpawnOperations = {
	    .file_actions_init         = call_posix_spawn_file_actions_init,
	    .file_actions_addclosefrom = call_posix_spawn_file_actions_addclosefrom,
	    .file_actions_destroy      = call_posix_spawn_file_actions_destroy,
	    .spawn                     = call_posix_spawn,
	};

	auto spawn_compare_process(const howdy::pam::CompareLaunchRequest &request, pid_t *child_pid,
	                           const PosixSpawnOperations &operations, void *context) -> int {
		const std::string config_path(request.config_path);
		const std::string username(request.username);

		std::array<char *, 5> args = {
		    const_cast<char *>(kCompareProcessPath),
		    const_cast<char *>("--config"),
		    const_cast<char *>(config_path.data()),
		    const_cast<char *>(username.data()),
		    nullptr,
		};

		const std::string user_models_env =
		    "HOWDY_USER_MODELS_DIR=" + std::string(request.user_models_dir);

		std::array<char *, 2> runtime_env = {
		    const_cast<char *>(user_models_env.data()),
		    nullptr,
		};
		std::array<char *, 1> empty_env = {
		    nullptr,
		};

		char **compare_env = request.staged_runtime ? runtime_env.data() : empty_env.data();

		posix_spawn_file_actions_t file_actions;
		const int init_result = operations.file_actions_init(context, &file_actions);
		if (init_result != 0) {
			return init_result;
		}

		const int closefrom_result =
		    operations.file_actions_addclosefrom(context, &file_actions, STDERR_FILENO + 1);
		if (closefrom_result != 0) {
			(void)operations.file_actions_destroy(context, &file_actions);
			return closefrom_result;
		}

		const int spawn_result = operations.spawn({.context   = context,
		                                           .child_pid = child_pid,
		                                           .path      = kCompareProcessPath,
		                                           .actions   = &file_actions,
		                                           .argv      = args.data(),
		                                           .envp      = compare_env});
		(void)operations.file_actions_destroy(context, &file_actions);
		return spawn_result;
	}

	auto spawn_compare_process_dependency(void                                   *context,
	                                      const howdy::pam::CompareLaunchRequest &request,
	                                      pid_t *child_pid) -> int {
		(void)context;
		return spawn_compare_process(request, child_pid, kPosixSpawnOperations, nullptr);
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
		    .native_prompt = native_prompt_ ? &*native_prompt_ : nullptr,
		};
	}

	auto PromptCoordinator::valid() const -> bool {
		return dependencies_.spawn_compare_process != nullptr &&
		       dependencies_.wait_for_compare_process != nullptr &&
		       dependencies_.terminate_compare != nullptr &&
		       dependencies_.input_prompt_preflight != nullptr &&
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
			native_prompt_.emplace(pamh_);
			if (!native_prompt_->available()) {
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

		if (effective_workaround_ == Workaround::Input && ask_auth_tok_ && !existing_auth_token_ &&
		    !dependencies_.input_prompt_preflight(dependencies_.context)) {
			syslog(LOG_WARNING,
			       "Input prompt workaround preflight failed; falling back to standard PAM prompt");
			effective_workaround_ = Workaround::Off;
		}
		return effective_workaround_ == Workaround::Native
		           ? native_prompt_.has_value() && !existing_auth_token_
		           : should_ask_for_password(ask_auth_tok_, effective_workaround_,
		                                     existing_auth_token_);
	}

	auto PromptCoordinator::start_password_task(bool ask_pass)
	    -> optional_task<std::tuple<int, char *>> & {
		auto &task = pass_task_.emplace([this] -> std::tuple<int, char *> {
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

	auto PromptCoordinator::run(const CompareLaunchRequest &request) -> PromptCoordinatorResult {
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
		auto      *native_prompt = native_prompt_.has_value() ? &native_prompt_.value() : nullptr;
		const auto stop_result = request_password_prompt_stop(pass_task, stop_plan, native_prompt);
		if (!stop_result.prompt_stopped) {
			syslog(LOG_ERR, "Input prompt workaround cancellation failed; waiting for "
			                "user/password prompt to complete");
			pass_task.stop();
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
		    .spawn_compare_process    = spawn_compare_process_dependency,
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
		return ::wait_for_compare_process(child_pid, std::chrono::steady_clock::now() +
		                                                 std::chrono::seconds(1));
	}

	auto wait_for_compare_process(pid_t child_pid, std::chrono::steady_clock::duration hard_timeout)
	    -> int {
		return ::wait_for_compare_process(child_pid,
		                                  std::chrono::steady_clock::now() + hard_timeout);
	}

	auto spawn_compare_process(const CompareLaunchRequest &request, pid_t *child_pid,
	                           const PosixSpawnOperations &operations, void *context) -> int {
		const ::PosixSpawnOperations internal_operations = {
		    .file_actions_init         = operations.file_actions_init,
		    .file_actions_addclosefrom = operations.file_actions_addclosefrom,
		    .file_actions_destroy      = operations.file_actions_destroy,
		    .spawn                     = operations.spawn,
		};
		return ::spawn_compare_process(request, child_pid, internal_operations, context);
	}

}  // namespace howdy::pam::testing
#endif
