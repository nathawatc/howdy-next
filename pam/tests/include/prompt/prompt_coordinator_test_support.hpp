#pragma once
#include "prompt/prompt_coordinator.hpp"
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

namespace howdy::test::prompt_coordinator {

	using howdy::test::expect;

	using howdy::native::CompareExit;
	using howdy::pam::PromptCoordinator;
	using howdy::pam::PromptCoordinatorDecision;
	using howdy::pam::PromptCoordinatorDependencies;
	using namespace std::chrono_literals;

	struct FakeContext {
		std::atomic<int>          spawn_calls{0};
		std::atomic<pid_t>        spawned_pid{-1};
		std::atomic<int>          wait_calls{0};
		std::atomic<pid_t>        waited_pid{-1};
		std::atomic<int>          last_wait_status{0};
		std::atomic<int>          terminate_calls{0};
		std::atomic<pid_t>        terminated_pid{-1};
		std::atomic<int>          preflight_calls{0};
		std::atomic<int>          enter_device_constructions{0};
		std::atomic<int>          enter_presses{0};
		std::atomic<int>          auth_token_calls{0};
		int                       token_result = PAM_SUCCESS;
		std::chrono::milliseconds token_delay{0};
		bool                      block_token_until_warning = false;
		bool                      warning_released_token    = false;
		std::mutex                token_mutex;
		std::condition_variable   token_condition;
		bool                      preflight_result         = true;
		bool                      fail_enter_construction  = false;
		bool                      return_null_enter_device = false;
		bool                      fail_enter_send          = false;
		bool                      release_token_on_enter   = false;
		bool                      request_native_prompt    = false;
		bool                      complete_native_prompt   = false;
		bool                      native_available         = true;
		int                       native_install_result    = PAM_SUCCESS;
		std::atomic<int>          native_abort_calls{0};
		std::atomic<int>          native_restore_calls{0};
		int                       prompt_master_fd = -1;
		std::atomic<bool>         native_prompt_seen{false};
		std::atomic<bool>         native_prompt_installed{false};
		std::atomic<bool>         native_prompt_input_sent{false};
		std::atomic<bool>         native_prompt_completed{false};
		std::atomic<bool>         pam_completion_observed_by_waiter{false};
		std::atomic<int>          original_conversation_calls{0};
		int                       spawn_result = 0;
		std::string               spawned_config_path;
		std::string               spawned_username;
		std::string               spawned_user_models_dir;
		bool                      spawned_staged_runtime = false;
		pid_t                     next_child_pid         = -1;
		std::mutex                native_prompt_mutex;
		std::condition_variable   native_prompt_condition;
	};

	class FakeEnterDevice final : public EnterDevice {
	public:
		explicit FakeEnterDevice(FakeContext *context)
		    : context_(context) {}

		void send_enter_press() override {
			++context_->enter_presses;
			if (context_->fail_enter_send) {
				throw std::runtime_error("Failed to send Enter keypress");
			}
			if (context_->release_token_on_enter) {
				{
					std::unique_lock<std::mutex> lock(context_->token_mutex);
					context_->warning_released_token = true;
				}
				context_->token_condition.notify_one();
			}
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

		auto install() -> int override {
			if (context_->native_install_result == PAM_SUCCESS) {
				context_->native_prompt_installed = true;
			}
			return context_->native_install_result;
		}

		void request_abort() override {
			++context_->native_abort_calls;
			context_->native_prompt_completed = true;
			context_->native_prompt_condition.notify_one();
		}

		void restore_original() override {
			++context_->native_restore_calls;
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
		int enter     = 0;
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

	inline void wait_for_native_prompt_completion(FakeContext &fake) {
		if (fake.complete_native_prompt) {
			std::unique_lock<std::mutex> lock(fake.native_prompt_mutex);
			fake.native_prompt_condition.wait(lock, [&fake] -> bool {
				return fake.native_prompt_completed.load();
			});
			fake.pam_completion_observed_by_waiter = true;
		}
	}

	inline auto wait_for_compare(void *context, pid_t child_pid,
	                             [[maybe_unused]] std::chrono::steady_clock::time_point deadline)
	    -> int {
		auto &fake = *static_cast<FakeContext *>(context);
		++fake.wait_calls;
		fake.waited_pid = child_pid;
		if (fake.request_native_prompt) {
			wait_for_native_prompt_completion(fake);
		}
		while (true) {
			int         status = 0;
			const pid_t result = waitpid(child_pid, &status, 0);
			if (result == child_pid) {
				fake.last_wait_status = status;
				return status;
			}
			if (result < 0 && errno == EINTR) {
				continue;
			}
			return static_cast<int>(CompareExit::kAbort) << 8;
		}
	}

	inline auto watchdog_wait_for_compare(void *context, pid_t child_pid,
	                                      std::chrono::steady_clock::time_point deadline) -> int {
		auto &fake = *static_cast<FakeContext *>(context);
		++fake.wait_calls;
		fake.waited_pid      = child_pid;
		const auto remaining = std::max(deadline - std::chrono::steady_clock::now(),
		                                std::chrono::steady_clock::duration::zero());
		const int  status    = howdy::pam::compare_process::wait_until(
		    child_pid, std::chrono::steady_clock::now() + remaining);
		fake.last_wait_status = status;
		return status;
	}

	inline auto terminate_compare(void *context, pid_t child_pid) -> void {
		auto &fake = *static_cast<FakeContext *>(context);
		++fake.terminate_calls;
		fake.terminated_pid = child_pid;
		(void)kill(child_pid, SIGTERM);
	}

	inline auto input_preflight(void *context) -> bool {
		auto &fake = *static_cast<FakeContext *>(context);
		++fake.preflight_calls;
		return fake.preflight_result;
	}

	inline auto create_enter_device(void *context) -> std::unique_ptr<EnterDevice> {
		auto &fake = *static_cast<FakeContext *>(context);
		++fake.enter_device_constructions;
		if (fake.fail_enter_construction) {
			throw std::runtime_error("Failed to create uinput device");
		}
		if (fake.return_null_enter_device) {
			return nullptr;
		}
		return std::make_unique<FakeEnterDevice>(&fake);
	}

	inline auto create_native_prompt(void *context, pam_handle_t *pamh)
	    -> std::unique_ptr<NativePrompt> {
		(void)pamh;
		auto &fake = *static_cast<FakeContext *>(context);
		return std::make_unique<FakeNativePrompt>(&fake);
	}

	inline auto request_auth_token(void *context, pam_handle_t *pamh)
	    -> std::tuple<int, const char *> {
		auto &fake = *static_cast<FakeContext *>(context);
		++fake.auth_token_calls;
		if (fake.request_native_prompt) {
			(void)pamh;
			fake.native_prompt_seen = true;
			if (fake.complete_native_prompt) {
				fake.native_prompt_input_sent = true;
				fake.native_prompt_completed  = true;
				fake.native_prompt_condition.notify_one();
			} else {
				std::unique_lock<std::mutex> lock(fake.native_prompt_mutex);
				fake.native_prompt_condition.wait(lock, [&fake] -> bool {
					return fake.native_prompt_completed.load();
				});
			}
			return {PAM_SUCCESS, nullptr};
		}
		if (fake.block_token_until_warning) {
			std::unique_lock<std::mutex> lock(fake.token_mutex);
			fake.token_condition.wait_for(lock, 1s, [&fake] -> bool {
				return fake.warning_released_token;
			});
		}
		std::this_thread::sleep_for(fake.token_delay);
		return {fake.token_result, nullptr};
	}

	inline auto dependencies(FakeContext *context) -> PromptCoordinatorDependencies {
		return PromptCoordinatorDependencies{
		    .context                  = context,
		    .spawn_compare_process    = spawn_compare_process,
		    .wait_for_compare_process = wait_for_compare,
		    .terminate_compare        = terminate_compare,
		    .input_prompt_preflight   = input_preflight,
		    .create_enter_device      = create_enter_device,
		    .create_native_prompt     = create_native_prompt,
		    .request_auth_token       = request_auth_token,
		};
	}

	inline auto callback_counts(const FakeContext &context) -> CallbackCounts {
		return CallbackCounts{
		    .spawn     = context.spawn_calls.load(),
		    .wait      = context.wait_calls.load(),
		    .terminate = context.terminate_calls.load(),
		    .preflight = context.preflight_calls.load(),
		    .enter     = context.enter_device_constructions.load(),
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
				(void)waitpid(child_pid, nullptr, 0);
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
