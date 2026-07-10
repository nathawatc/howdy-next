#include "common/compare_exit.hpp"
#include "paths.hpp"
#include "prompt_coordinator.hpp"
#include "prompt_coordinator_testing.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <mutex>
#include <poll.h>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <unistd.h>
#include <vector>

#include <security/pam_appl.h>

#include <sys/wait.h>

namespace {

	using howdy::native::CompareExit;
	using howdy::pam::PromptCoordinator;
	using howdy::pam::PromptCoordinatorDecision;
	using howdy::pam::PromptCoordinatorDependencies;

	struct FakeContext {
		std::atomic<int>          spawn_calls{0};
		std::atomic<pid_t>        spawned_pid{-1};
		std::atomic<int>          wait_calls{0};
		std::atomic<pid_t>        waited_pid{-1};
		std::atomic<int>          terminate_calls{0};
		std::atomic<pid_t>        terminated_pid{-1};
		std::atomic<int>          preflight_calls{0};
		std::atomic<int>          auth_token_calls{0};
		int                       token_result = PAM_SUCCESS;
		std::chrono::milliseconds token_delay{0};
		bool                      preflight_result       = true;
		bool                      request_native_prompt  = false;
		bool                      complete_native_prompt = false;
		int                       prompt_master_fd       = -1;
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
		int                      calls        = 0;
		int                      spawn_result = 0;
		pid_t                    next_pid     = 4242;
		std::string              path;
		std::vector<std::string> argv;
		std::vector<std::string> environment;
	};

	auto capture_posix_spawn(void *context, pid_t *child_pid, const char *path, char *const *argv,
	                         char *const *envp) -> int {
		auto &capture = *static_cast<PosixSpawnCapture *>(context);
		++capture.calls;
		capture.path = path;
		for (char *const *argument = argv; *argument != nullptr; ++argument) {
			capture.argv.emplace_back(*argument);
		}
		for (char *const *entry = envp; *entry != nullptr; ++entry) {
			capture.environment.emplace_back(*entry);
		}

		if (capture.spawn_result == 0) {
			*child_pid = capture.next_pid;
		}
		return capture.spawn_result;
	}

	auto original_conversation(int num_msg, const struct pam_message **messages,
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

	class ScopedNativePromptResults {
	public:
		ScopedNativePromptResults(int available_result, int install_result) {
			NativePromptConversation::set_test_available_result(available_result);
			NativePromptConversation::set_test_install_result(install_result);
		}

		ScopedNativePromptResults(const ScopedNativePromptResults &)                     = delete;
		auto operator=(const ScopedNativePromptResults &) -> ScopedNativePromptResults & = delete;

		~ScopedNativePromptResults() {
			NativePromptConversation::set_test_available_result(-1);
			NativePromptConversation::set_test_install_result(-1);
		}
	};

	struct CallbackCounts {
		int spawn     = 0;
		int wait      = 0;
		int terminate = 0;
		int preflight = 0;
		int auth      = 0;

		auto operator==(const CallbackCounts &) const -> bool = default;
	};

	auto expect(bool condition, const std::string &message) -> bool {
		if (!condition) {
			std::cerr << "FAIL: " << message << "\n";
			return false;
		}
		return true;
	}

	auto spawn_compare_process(void *context, const howdy::pam::CompareLaunchRequest &request,
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

	auto wait_for_compare(void *context, pid_t child_pid) -> int {
		auto &fake = *static_cast<FakeContext *>(context);
		++fake.wait_calls;
		fake.waited_pid = child_pid;
		if (fake.request_native_prompt && fake.prompt_master_fd >= 0) {
			struct pollfd prompt_fd = {
			    .fd      = fake.prompt_master_fd,
			    .events  = POLLIN,
			    .revents = 0,
			};
			int poll_result = -1;
			do {
				poll_result = poll(&prompt_fd, 1, -1);
			} while (poll_result < 0 && errno == EINTR);
			if (poll_result > 0 && (prompt_fd.revents & POLLIN) != 0) {
				std::array<char, 64> buffer{};
				if (read(fake.prompt_master_fd, buffer.data(), buffer.size()) > 0) {
					fake.native_prompt_seen = true;
					if (fake.complete_native_prompt) {
						const std::string input   = "password\n";
						std::size_t       written = 0;
						while (written < input.size()) {
							const ssize_t result =
							    write(fake.prompt_master_fd, input.data() + written,
							          input.size() - written);
							if (result > 0) {
								written += static_cast<std::size_t>(result);
								continue;
							}
							if (result < 0 && errno == EINTR) {
								continue;
							}
							break;
						}
						fake.native_prompt_input_sent = written == input.size();
					}
				}
			}
			if (fake.complete_native_prompt) {
				std::unique_lock<std::mutex> lock(fake.native_prompt_mutex);
				fake.native_prompt_condition.wait(lock, [&fake] {
					return fake.native_prompt_completed.load();
				});
				fake.pam_completion_observed_by_waiter = true;
			}
		}
		while (true) {
			int         status = 0;
			const pid_t result = waitpid(child_pid, &status, 0);
			if (result == child_pid) {
				return status;
			}
			if (result < 0 && errno == EINTR) {
				continue;
			}
			return static_cast<int>(CompareExit::kAbort) << 8;
		}
	}

	auto terminate_compare(void *context, pid_t child_pid) -> void {
		auto &fake = *static_cast<FakeContext *>(context);
		++fake.terminate_calls;
		fake.terminated_pid = child_pid;
		(void)kill(child_pid, SIGTERM);
	}

	auto input_preflight(void *context) -> bool {
		auto &fake = *static_cast<FakeContext *>(context);
		++fake.preflight_calls;
		return fake.preflight_result;
	}

	auto request_auth_token(void *context, pam_handle_t *pamh) -> std::tuple<int, char *> {
		auto &fake = *static_cast<FakeContext *>(context);
		++fake.auth_token_calls;
		if (fake.request_native_prompt) {
			const void *item = nullptr;
			if (pam_get_item(pamh, PAM_CONV, &item) != PAM_SUCCESS || item == nullptr) {
				return {PAM_SYSTEM_ERR, nullptr};
			}
			const auto *conversation = static_cast<const struct pam_conv *>(item);
			fake.native_prompt_installed =
			    conversation->conv != original_conversation || conversation->appdata_ptr != context;
			const struct pam_message message = {
			    .msg_style = PAM_PROMPT_ECHO_OFF,
			    .msg       = "Password: ",
			};
			const struct pam_message *message_ptr = &message;
			struct pam_response      *response    = nullptr;
			const int                 result =
			    conversation->conv(1, &message_ptr, &response, conversation->appdata_ptr);
			if (response != nullptr) {
				if (response->resp != nullptr) {
					std::memset(response->resp, 0, std::strlen(response->resp));
					std::free(response->resp);
				}
				std::free(response);
			}
			fake.native_prompt_completed = true;
			fake.native_prompt_condition.notify_one();
			return {result, nullptr};
		}
		std::this_thread::sleep_for(fake.token_delay);
		return {fake.token_result, nullptr};
	}

	auto dependencies(FakeContext *context) -> PromptCoordinatorDependencies {
		return PromptCoordinatorDependencies{
		    .context                  = context,
		    .spawn_compare_process    = spawn_compare_process,
		    .wait_for_compare_process = wait_for_compare,
		    .terminate_compare        = terminate_compare,
		    .input_prompt_preflight   = input_preflight,
		    .request_auth_token       = request_auth_token,
		};
	}

	auto callback_counts(const FakeContext &context) -> CallbackCounts {
		return CallbackCounts{
		    .spawn     = context.spawn_calls.load(),
		    .wait      = context.wait_calls.load(),
		    .terminate = context.terminate_calls.load(),
		    .preflight = context.preflight_calls.load(),
		    .auth      = context.auth_token_calls.load(),
		};
	}

	auto make_compare_request(std::string_view config_path     = "/etc/howdy/config.ini",
	                          std::string_view username        = "alice",
	                          std::string_view user_models_dir = "/etc/howdy/models",
	                          bool staged_runtime = false) -> howdy::pam::CompareLaunchRequest {
		return {
		    .config_path     = std::string(config_path),
		    .username        = std::string(username),
		    .user_models_dir = std::string(user_models_dir),
		    .staged_runtime  = staged_runtime,
		};
	}

	auto spawn_child(int exit_code, std::chrono::milliseconds delay = {}) -> pid_t {
		const pid_t child_pid = fork();
		if (child_pid == 0) {
			std::this_thread::sleep_for(delay);
			_exit(exit_code);
		}
		return child_pid;
	}

	auto spawn_blocked_child() -> pid_t {
		const pid_t child_pid = fork();
		if (child_pid == 0) {
			while (true) {
				pause();
			}
		}
		return child_pid;
	}

	auto child_reaped(pid_t child_pid) -> bool {
		errno                   = 0;
		const pid_t wait_result = waitpid(child_pid, nullptr, WNOHANG);
		return wait_result == -1 && errno == ECHILD;
	}

	auto test_compare_wins_without_password_prompt() -> bool {
		FakeContext context;
		const pid_t child_pid = spawn_child(EXIT_SUCCESS);
		if (!expect(child_pid > 0, "compare-winner child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;

		PromptCoordinator coordinator(nullptr, Workaround::Off, false, false,
		                              dependencies(&context));
		const auto        result = coordinator.run(make_compare_request());
		const bool        reaped = child_reaped(child_pid);
		return expect(result.decision == PromptCoordinatorDecision::kHowdyResult,
		              "compare winner returns Howdy result") &&
		       expect(context.spawn_calls == 1 && context.spawned_pid == child_pid,
		              "compare winner spawns child once") &&
		       expect(result.compare_status == 0,
		              "compare winner preserves exact successful wait status") &&
		       expect(context.wait_calls == 1 && context.waited_pid == child_pid,
		              "compare winner waits once for child") &&
		       expect(context.auth_token_calls == 0,
		              "compare winner does not request disabled password") &&
		       expect(context.terminate_calls == 0, "compare winner does not terminate child") &&
		       expect(reaped, "compare winner reaps child");
	}

	auto test_pam_wins() -> bool {
		FakeContext context;
		const pid_t child_pid = spawn_child(EXIT_SUCCESS, std::chrono::seconds(2));
		if (!expect(child_pid > 0, "PAM-winner child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;

		PromptCoordinator coordinator(nullptr, Workaround::Input, true, false,
		                              dependencies(&context));
		const auto        result = coordinator.run(make_compare_request());
		const bool        reaped = child_reaped(child_pid);
		return expect(result.decision == PromptCoordinatorDecision::kPamResult,
		              "PAM winner returns PAM result") &&
		       expect(context.spawn_calls == 1 && context.spawned_pid == child_pid,
		              "PAM winner spawns child once") &&
		       expect(context.wait_calls == 1 && context.waited_pid == child_pid,
		              "PAM winner waits for spawned child") &&
		       expect(result.pam_status == PAM_SUCCESS, "PAM winner preserves PAM success") &&
		       expect(context.preflight_calls == 1, "PAM winner runs input preflight once") &&
		       expect(context.auth_token_calls == 1, "PAM winner requests token once") &&
		       expect(context.terminate_calls == 1 && context.terminated_pid == child_pid,
		              "PAM winner terminates compare child once") &&
		       expect(reaped, "PAM winner reaps compare child");
	}

	auto test_compare_failure_password_result(int pam_result, const std::string &label) -> bool {
		FakeContext context{
		    .token_result = pam_result,
		    .token_delay  = std::chrono::milliseconds(100),
		};
		const pid_t child_pid = spawn_child(static_cast<int>(CompareExit::kTimeoutReached));
		if (!expect(child_pid > 0, label + " child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;

		PromptCoordinator coordinator(nullptr, Workaround::Input, true, false,
		                              dependencies(&context));
		const auto        result          = coordinator.run(make_compare_request());
		const int         expected_status = static_cast<int>(CompareExit::kTimeoutReached) << 8;
		const bool        reaped          = child_reaped(child_pid);
		return expect(result.decision == PromptCoordinatorDecision::kPasswordFallback,
		              label + " returns password fallback") &&
		       expect(context.spawn_calls == 1 && context.spawned_pid == child_pid,
		              label + " spawns child once") &&
		       expect(context.wait_calls == 1 && context.waited_pid == child_pid,
		              label + " waits for spawned child") &&
		       expect(result.compare_status == expected_status,
		              label + " preserves exact compare status") &&
		       expect(result.pam_status == pam_result, label + " preserves PAM result") &&
		       expect(context.auth_token_calls == 1, label + " requests token once") &&
		       expect(context.terminate_calls == 0, label + " does not terminate child") &&
		       expect(reaped, label + " reaps child");
	}

	auto test_input_preflight_fallback() -> bool {
		FakeContext context{.preflight_result = false};
		const pid_t child_pid = spawn_child(EXIT_SUCCESS);
		if (!expect(child_pid > 0, "preflight-fallback child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;

		PromptCoordinator coordinator(nullptr, Workaround::Input, true, false,
		                              dependencies(&context));
		const auto        result = coordinator.run(make_compare_request());
		const bool        reaped = child_reaped(child_pid);
		return expect(result.decision == PromptCoordinatorDecision::kHowdyResult,
		              "preflight fallback returns compare result") &&
		       expect(context.spawn_calls == 1 && context.spawned_pid == child_pid,
		              "preflight fallback spawns child once") &&
		       expect(context.wait_calls == 1 && context.waited_pid == child_pid,
		              "preflight fallback waits for spawned child") &&
		       expect(context.preflight_calls == 1, "preflight fallback checks input once") &&
		       expect(context.auth_token_calls == 0,
		              "off fallback preserves standard non-parallel password behavior") &&
		       expect(context.terminate_calls == 0,
		              "preflight fallback does not terminate compare child") &&
		       expect(reaped, "preflight fallback reaps child");
	}

	auto test_native_setup_without_input_fallback(int available_result, int install_result,
	                                              const std::string &label) -> bool {
		FakeContext      context;
		NativePamFixture fixture(&context);
		if (!expect(fixture.start(false), label + " starts PAM handle")) {
			return false;
		}
		ScopedNativePromptResults prompt_results(available_result, install_result);
		const pid_t child_pid = spawn_child(static_cast<int>(CompareExit::kTimeoutReached));
		if (!expect(child_pid > 0, label + " child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;

		PromptCoordinator coordinator(fixture.pamh(), Workaround::Native, true, false,
		                              dependencies(&context));
		const auto        result = coordinator.run(make_compare_request());
		return expect(result.decision == PromptCoordinatorDecision::kHowdyResult,
		              label + " returns compare result without input fallback") &&
		       expect(context.spawn_calls == 1 && context.spawned_pid == child_pid,
		              label + " spawns child once") &&
		       expect(context.wait_calls == 1 && context.waited_pid == child_pid,
		              label + " waits for spawned child") &&
		       expect(context.preflight_calls == 0, label + " skips input preflight") &&
		       expect(context.auth_token_calls == 0, label + " does not request token") &&
		       expect(context.terminate_calls == 0, label + " does not terminate child") &&
		       expect(context.original_conversation_calls == 0,
		              label + " does not invoke original conversation") &&
		       expect(child_reaped(child_pid), label + " reaps child");
	}

	auto test_native_input_success_uses_native_path() -> bool {
		FakeContext      context;
		NativePamFixture fixture(&context);
		if (!expect(fixture.start(false), "native-input success starts PAM handle")) {
			return false;
		}
		ScopedNativePromptResults prompt_results(1, PAM_SUCCESS);
		const pid_t               child_pid = spawn_blocked_child();
		if (!expect(child_pid > 0, "native-input success child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;

		PromptCoordinator coordinator(fixture.pamh(), Workaround::NativeInput, true, false,
		                              dependencies(&context));
		const auto        result = coordinator.run(make_compare_request());
		return expect(result.decision == PromptCoordinatorDecision::kPamResult,
		              "native-input success uses native password task") &&
		       expect(result.pam_status == PAM_SUCCESS,
		              "native-input success preserves PAM success") &&
		       expect(context.spawn_calls == 1 && context.spawned_pid == child_pid,
		              "native-input success spawns child once") &&
		       expect(context.wait_calls == 1 && context.waited_pid == child_pid,
		              "native-input success waits for spawned child") &&
		       expect(context.preflight_calls == 0, "native-input success skips input preflight") &&
		       expect(context.auth_token_calls == 1, "native-input success requests token once") &&
		       expect(context.terminate_calls == 1 && context.terminated_pid == child_pid,
		              "native-input success terminates blocked compare child once") &&
		       expect(context.original_conversation_calls == 0,
		              "native-input success does not invoke original conversation") &&
		       expect(child_reaped(child_pid), "native-input success reaps compare child");
	}

	auto test_native_input_setup_fallback(int available_result, int install_result,
	                                      const std::string &label) -> bool {
		FakeContext context{
		    .token_result = PAM_SUCCESS,
		    .token_delay  = std::chrono::milliseconds(100),
		};
		NativePamFixture fixture(&context);
		if (!expect(fixture.start(false), label + " starts PAM handle")) {
			return false;
		}
		ScopedNativePromptResults prompt_results(available_result, install_result);
		const pid_t child_pid = spawn_child(static_cast<int>(CompareExit::kTimeoutReached));
		if (!expect(child_pid > 0, label + " child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;

		PromptCoordinator coordinator(fixture.pamh(), Workaround::NativeInput, true, false,
		                              dependencies(&context));
		const auto        result = coordinator.run(make_compare_request());
		return expect(result.decision == PromptCoordinatorDecision::kPasswordFallback,
		              label + " falls back to input password task") &&
		       expect(context.spawn_calls == 1 && context.spawned_pid == child_pid,
		              label + " spawns child once") &&
		       expect(context.wait_calls == 1 && context.waited_pid == child_pid,
		              label + " waits for spawned child") &&
		       expect(context.preflight_calls == 1, label + " runs input preflight once") &&
		       expect(context.auth_token_calls == 1, label + " requests token once") &&
		       expect(context.terminate_calls == 0, label + " does not terminate child") &&
		       expect(context.original_conversation_calls == 0,
		              label + " does not invoke original conversation") &&
		       expect(child_reaped(child_pid), label + " reaps child");
	}

	auto test_cleanup_restores_after_stopped_task() -> bool {
		FakeContext      context;
		NativePamFixture fixture(&context);
		if (!expect(fixture.start(true), "stopped-task cleanup starts PAM PTY")) {
			return false;
		}

		NativePromptConversation native_prompt(fixture.pamh());
		if (!expect(native_prompt.available(), "stopped-task cleanup native prompt available") ||
		    !expect(native_prompt.install() == PAM_SUCCESS,
		            "stopped-task cleanup installs native conversation")) {
			return false;
		}

		optional_task<std::tuple<int, char *>> pass_task([] {
			return std::tuple<int, char *>(PAM_SUCCESS, nullptr);
		});
		pass_task.activate();
		pass_task.stop();
		if (!expect(!pass_task.active(), "stopped-task cleanup task is inactive after stop")) {
			return false;
		}

		howdy::pam::testing::cleanup_native_prompt(pass_task, native_prompt);
		return expect(fixture.original_conversation_restored(),
		              "cleanup restores original conversation after task becomes inactive");
	}

	auto test_native_blocked_prompt_cleanup() -> bool {
		FakeContext      context{.request_native_prompt = true};
		NativePamFixture fixture(&context);
		if (!expect(fixture.start(true), "blocked native prompt starts PAM PTY")) {
			return false;
		}
		const pid_t child_pid = spawn_child(EXIT_SUCCESS);
		if (!expect(child_pid > 0, "blocked native prompt child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;

		howdy::pam::PromptCoordinatorResult result;
		{
			PromptCoordinator coordinator(fixture.pamh(), Workaround::Native, true, false,
			                              dependencies(&context));
			result = coordinator.run(make_compare_request());
		}

		return expect(result.decision == PromptCoordinatorDecision::kHowdyResult,
		              "blocked native prompt returns compare result") &&
		       expect(context.spawn_calls == 1 && context.spawned_pid == child_pid,
		              "blocked native prompt spawns child once") &&
		       expect(context.wait_calls == 1 && context.waited_pid == child_pid,
		              "blocked native prompt waits for spawned child") &&
		       expect(result.compare_status == 0,
		              "blocked native prompt preserves successful compare status") &&
		       expect(result.prompt_stopped, "blocked native prompt task joins after abort") &&
		       expect(context.native_prompt_seen,
		              "blocked native prompt reaches native conversation") &&
		       expect(context.auth_token_calls == 1, "blocked native prompt requests token once") &&
		       expect(context.terminate_calls == 0,
		              "blocked native prompt does not terminate compare child") &&
		       expect(context.original_conversation_calls == 0,
		              "blocked native prompt uses installed conversation") &&
		       expect(fixture.original_conversation_restored(),
		              "coordinator destructor restores native conversation") &&
		       expect(child_reaped(child_pid), "blocked native prompt reaps child");
	}

	auto test_native_pam_wins() -> bool {
		FakeContext context{
		    .request_native_prompt  = true,
		    .complete_native_prompt = true,
		};
		NativePamFixture fixture(&context);
		if (!expect(fixture.start(true), "native PAM winner starts PAM PTY")) {
			return false;
		}
		const pid_t child_pid = spawn_blocked_child();
		if (!expect(child_pid > 0, "native PAM winner child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;

		howdy::pam::PromptCoordinatorResult result;
		{
			PromptCoordinator coordinator(fixture.pamh(), Workaround::Native, true, false,
			                              dependencies(&context));
			result = coordinator.run(make_compare_request());
		}
		return expect(result.decision == PromptCoordinatorDecision::kPamResult,
		              "native PAM winner returns PAM result") &&
		       expect(context.spawn_calls == 1 && context.spawned_pid == child_pid,
		              "native PAM winner spawns child once") &&
		       expect(context.wait_calls == 1 && context.waited_pid == child_pid,
		              "native PAM winner waits for spawned child") &&
		       expect(result.pam_status == PAM_SUCCESS,
		              "native PAM winner preserves PAM success") &&
		       expect(context.native_prompt_installed,
		              "native PAM winner observes installed native conversation") &&
		       expect(context.native_prompt_seen,
		              "native PAM winner observes native password prompt") &&
		       expect(context.native_prompt_input_sent,
		              "native PAM winner sends password input after prompt observation") &&
		       expect(context.native_prompt_completed,
		              "native PAM winner completes native conversation") &&
		       expect(context.pam_completion_observed_by_waiter,
		              "native PAM winner synchronizes completion before compare wait") &&
		       expect(context.auth_token_calls == 1,
		              "native PAM winner completes password task once") &&
		       expect(context.preflight_calls == 0, "native PAM winner skips input preflight") &&
		       expect(context.terminate_calls == 1 && context.terminated_pid == child_pid,
		              "native PAM winner terminates blocked compare child once") &&
		       expect(child_reaped(child_pid), "native PAM winner reaps compare child") &&
		       expect(fixture.original_conversation_restored(),
		              "native PAM winner restores original conversation after destruction") &&
		       expect(context.original_conversation_calls == 0,
		              "native PAM winner leaves no blocked native prompt task");
	}

	auto test_launch_request(const howdy::pam::CompareLaunchRequest &request,
	                         const std::string                      &expected_config_path,
	                         const std::string                      &expected_username,
	                         const std::string                      &expected_user_models_dir,
	                         bool expected_staged_runtime, const std::string &label) -> bool {
		FakeContext context;
		const pid_t child_pid = spawn_child(EXIT_SUCCESS);
		if (!expect(child_pid > 0, label + " child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;

		PromptCoordinator coordinator(nullptr, Workaround::Off, false, false,
		                              dependencies(&context));
		const auto        result = coordinator.run(request);
		return expect(result.decision == PromptCoordinatorDecision::kHowdyResult,
		              label + " returns Howdy result") &&
		       expect(context.spawn_calls == 1 && context.spawned_pid == child_pid,
		              label + " spawns child once") &&
		       expect(context.wait_calls == 1 && context.waited_pid == child_pid,
		              label + " waits for spawned child") &&
		       expect(context.spawned_config_path == expected_config_path,
		              label + " preserves config path") &&
		       expect(context.spawned_username == expected_username,
		              label + " preserves username") &&
		       expect(context.spawned_user_models_dir == expected_user_models_dir,
		              label + " preserves models directory") &&
		       expect(context.spawned_staged_runtime == expected_staged_runtime,
		              label + " preserves staged-runtime selection") &&
		       expect(child_reaped(child_pid), label + " reaps child");
	}

	auto test_direct_runtime_launch_request() -> bool {
		return test_launch_request(
		    make_compare_request("/etc/howdy/config.ini", "alice", "/etc/howdy/models", false),
		    "/etc/howdy/config.ini", "alice", "/etc/howdy/models", false, "direct runtime request");
	}

	auto test_staged_runtime_launch_request() -> bool {
		return test_launch_request(make_compare_request("/run/howdy/runtime/config.ini", "alice",
		                                                "/run/howdy/runtime/models", true),
		                           "/run/howdy/runtime/config.ini", "alice",
		                           "/run/howdy/runtime/models", true, "staged runtime request");
	}

	auto test_production_spawn_adapter(const howdy::pam::CompareLaunchRequest &request,
	                                   const std::vector<std::string>         &expected_argv,
	                                   const std::vector<std::string>         &expected_environment,
	                                   const std::string                      &label) -> bool {
		PosixSpawnCapture capture;
		pid_t             child_pid = -1;
		const int         result    = howdy::pam::testing::spawn_compare_process(
		    request, &child_pid, capture_posix_spawn, &capture);

		return expect(result == 0, label + " returns spawn success") &&
		       expect(capture.calls == 1, label + " calls posix_spawn once") &&
		       expect(child_pid == capture.next_pid, label + " preserves spawned PID") &&
		       expect(capture.path == kCompareProcessPath, label + " preserves executable path") &&
		       expect(capture.argv == expected_argv, label + " preserves exact argv") &&
		       expect(capture.environment == expected_environment,
		              label + " preserves exact environment");
	}

	auto test_production_direct_runtime_environment() -> bool {
		return test_production_spawn_adapter(
		    make_compare_request("/etc/howdy/config.ini", "alice", "/etc/howdy/models", false),
		    {kCompareProcessPath, "--config", "/etc/howdy/config.ini", "alice"}, {},
		    "production direct runtime");
	}

	auto test_production_staged_runtime_environment() -> bool {
		return test_production_spawn_adapter(
		    make_compare_request("/run/howdy/runtime/config.ini", "alice",
		                         "/run/howdy/runtime/models", true),
		    {kCompareProcessPath, "--config", "/run/howdy/runtime/config.ini", "alice"},
		    {"HOWDY_USER_MODELS_DIR=/run/howdy/runtime/models"}, "production staged runtime");
	}

	auto test_owned_launch_request_from_temporaries() -> bool {
		const howdy::pam::CompareLaunchRequest request = {
		    .config_path     = std::string("/run/howdy/temporary/config.ini"),
		    .username        = std::string("temporary-user"),
		    .user_models_dir = std::string("/run/howdy/temporary/models"),
		    .staged_runtime  = true,
		};

		return test_production_spawn_adapter(
		    request,
		    {kCompareProcessPath, "--config", "/run/howdy/temporary/config.ini", "temporary-user"},
		    {"HOWDY_USER_MODELS_DIR=/run/howdy/temporary/models"}, "owned temporary request");
	}

	auto test_production_spawn_failure() -> bool {
		PosixSpawnCapture capture;
		capture.spawn_result = EACCES;
		pid_t     child_pid  = -1;
		const int result     = howdy::pam::testing::spawn_compare_process(
		    make_compare_request(), &child_pid, capture_posix_spawn, &capture);

		return expect(result == EACCES, "production spawn failure preserves error") &&
		       expect(capture.calls == 1, "production spawn failure calls posix_spawn once") &&
		       expect(child_pid == -1, "production spawn failure leaves child PID unchanged") &&
		       expect(capture.path == kCompareProcessPath,
		              "production spawn failure preserves executable path") &&
		       expect(capture.argv == std::vector<std::string>{kCompareProcessPath, "--config",
		                                                       "/etc/howdy/config.ini", "alice"},
		              "production spawn failure preserves exact argv") &&
		       expect(capture.environment.empty(),
		              "production spawn failure preserves empty direct environment");
	}

	auto test_spawn_failure() -> bool {
		FakeContext       context{.spawn_result = EACCES};
		PromptCoordinator coordinator(nullptr, Workaround::Input, true, false,
		                              dependencies(&context));

		const auto result = coordinator.run(make_compare_request());
		return expect(result.decision == PromptCoordinatorDecision::kCompareSpawnFailed,
		              "spawn failure returns compare-spawn-failed result") &&
		       expect(callback_counts(context) == CallbackCounts{.spawn = 1},
		              "spawn failure invokes no downstream callbacks") &&
		       expect(context.spawned_pid == -1, "spawn failure creates no child task");
	}

	auto test_invalid_spawn_pid() -> bool {
		FakeContext context;
		context.next_child_pid = -1;
		PromptCoordinator coordinator(nullptr, Workaround::Input, true, false,
		                              dependencies(&context));

		const auto result = coordinator.run(make_compare_request());
		return expect(result.decision == PromptCoordinatorDecision::kCompareSpawnFailed,
		              "invalid spawn PID returns compare-spawn-failed result") &&
		       expect(callback_counts(context) == CallbackCounts{.spawn = 1},
		              "invalid spawn PID invokes no downstream callbacks");
	}

	auto test_one_shot_after_spawn_failure() -> bool {
		FakeContext       context{.spawn_result = EACCES};
		PromptCoordinator coordinator(nullptr, Workaround::Input, true, false,
		                              dependencies(&context));

		const auto first  = coordinator.run(make_compare_request());
		const auto second = coordinator.run(
		    make_compare_request("/different/config.ini", "bob", "/different/models", true));
		return expect(first.decision == PromptCoordinatorDecision::kCompareSpawnFailed,
		              "spawn-failure one-shot first run reports spawn failure") &&
		       expect(second.decision == PromptCoordinatorDecision::kAlreadyRun,
		              "spawn-failure one-shot second run is rejected") &&
		       expect(callback_counts(context) == CallbackCounts{.spawn = 1},
		              "spawn-failure one-shot invokes spawn only once");
	}

	auto test_invalid_dependencies() -> bool {
		bool ok = true;
		for (int missing = 0; missing < 5; ++missing) {
			FakeContext context;
			auto        deps = dependencies(&context);
			switch (missing) {
				case 0:
					deps.spawn_compare_process = nullptr;
					break;
				case 1:
					deps.wait_for_compare_process = nullptr;
					break;
				case 2:
					deps.terminate_compare = nullptr;
					break;
				case 3:
					deps.input_prompt_preflight = nullptr;
					break;
				case 4:
					deps.request_auth_token = nullptr;
					break;
				default:
					break;
			}

			PromptCoordinator coordinator(nullptr, Workaround::Input, true, false, deps);
			ok &= expect(!coordinator.valid(), "missing dependency is invalid");
			const auto before = callback_counts(context);
			const auto result = coordinator.run(make_compare_request());
			ok &= expect(result.decision == PromptCoordinatorDecision::kInvalidDependencies,
			             "invalid coordinator returns invalid-dependencies result");
			ok &= expect(callback_counts(context) == before,
			             "invalid coordinator invokes no callback");
		}
		return ok;
	}

	auto test_one_shot() -> bool {
		FakeContext context;
		const pid_t child_pid = spawn_child(EXIT_SUCCESS);
		if (!expect(child_pid > 0, "one-shot child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;

		PromptCoordinator coordinator(nullptr, Workaround::Off, false, false,
		                              dependencies(&context));
		const auto        first = coordinator.run(make_compare_request());
		if (!expect(first.decision == PromptCoordinatorDecision::kHowdyResult,
		            "one-shot first run succeeds")) {
			return false;
		}
		const auto before = callback_counts(context);
		const auto second = coordinator.run(make_compare_request("/different/config.ini"));
		return expect(second.decision == PromptCoordinatorDecision::kAlreadyRun,
		              "one-shot second run is rejected") &&
		       expect(context.spawn_calls == 1 && context.spawned_pid == child_pid,
		              "one-shot first run spawns child once") &&
		       expect(context.wait_calls == 1 && context.waited_pid == child_pid,
		              "one-shot first run waits for spawned child") &&
		       expect(callback_counts(context) == before,
		              "one-shot second run invokes no callback") &&
		       expect(child_reaped(child_pid), "one-shot first run reaps child");
	}

}  // namespace

auto main() -> int {
	bool ok = true;
	ok &= test_compare_wins_without_password_prompt();
	ok &= test_pam_wins();
	ok &= test_compare_failure_password_result(PAM_SUCCESS, "successful password fallback");
	ok &= test_compare_failure_password_result(PAM_CONV_ERR, "failed password fallback");
	ok &= test_input_preflight_fallback();
	ok &= test_native_setup_without_input_fallback(0, -1, "native unavailable");
	ok &= test_native_setup_without_input_fallback(1, PAM_CONV_ERR, "native install failure");
	ok &= test_native_input_success_uses_native_path();
	ok &= test_native_input_setup_fallback(0, -1, "native-input unavailable");
	ok &= test_native_input_setup_fallback(1, PAM_CONV_ERR, "native-input install failure");
	ok &= test_cleanup_restores_after_stopped_task();
	ok &= test_native_blocked_prompt_cleanup();
	ok &= test_native_pam_wins();
	ok &= test_direct_runtime_launch_request();
	ok &= test_staged_runtime_launch_request();
	ok &= test_production_direct_runtime_environment();
	ok &= test_production_staged_runtime_environment();
	ok &= test_owned_launch_request_from_temporaries();
	ok &= test_production_spawn_failure();
	ok &= test_spawn_failure();
	ok &= test_invalid_spawn_pid();
	ok &= test_one_shot_after_spawn_failure();
	ok &= test_invalid_dependencies();
	ok &= test_one_shot();
	return ok ? 0 : 1;
}
