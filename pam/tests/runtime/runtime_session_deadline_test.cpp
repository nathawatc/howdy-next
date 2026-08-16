#include "runtime/runtime_session_process_test_support.hpp"
#include "runtime/runtime_session_test_groups.hpp"

namespace {
	using namespace howdy::test::runtime_session;
	constexpr auto kStagedRoot = "/run/howdy/pam-test";

	auto integration_duplicate_fd(void *context, int fd, int minimum_fd) -> int {
		(void)context;
		return fcntl(fd, F_DUPFD_CLOEXEC, minimum_fd);
	}

	auto integration_actions_init(void *context, posix_spawn_file_actions_t *actions) -> int {
		(void)context;
		return posix_spawn_file_actions_init(actions);
	}

	auto integration_actions_adddup2(void *context, posix_spawn_file_actions_t *actions,
	                                 int source_fd, int target_fd) -> int {
		(void)context;
		return posix_spawn_file_actions_adddup2(actions, source_fd, target_fd);
	}

	auto integration_actions_addclose(void *context, posix_spawn_file_actions_t *actions, int fd)
	    -> int {
		(void)context;
		return posix_spawn_file_actions_addclose(actions, fd);
	}

	auto integration_actions_destroy(void *context, posix_spawn_file_actions_t *actions) -> int {
		(void)context;
		return posix_spawn_file_actions_destroy(actions);
	}

	auto integration_close(void *context, int fd) -> int {
		(void)context;
		return close(fd);
	}

	auto wait_for_ready_byte(int ready_fd, std::string_view name) -> bool;
	void terminate_and_reap_test_child(pid_t child_pid);

	auto real_pipe2(void *context, int *pipe_fds, int flags) -> int {
		(void)context;
		return pipe2(pipe_fds, flags);
	}

	struct StalledSpawnContext {
		std::array<int, 2>       ready_pipe = {-1, -1};
		std::vector<std::string> log_messages;
		pid_t                    spawned_pid = -1;
		int                      spawn_calls = 0;
	};

	void stalled_spawn_log(void *context, std::string_view message) {
		static_cast<StalledSpawnContext *>(context)->log_messages.emplace_back(message);
	}

	auto stalled_spawn(const howdy::pam::auth_helper_process::SpawnRequest &request) -> int {
		auto &stalled = *static_cast<StalledSpawnContext *>(request.context);
		++stalled.spawn_calls;
		const int saved_stdin = fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
		if (saved_stdin < 0) {
			return errno;
		}
		if (dup2(stalled.ready_pipe[1], STDIN_FILENO) < 0) {
			const int duplicate_error = errno;
			(void)close(saved_stdin);
			return duplicate_error;
		}
		const std::string     command    = "trap '' TERM; printf R >&0; exec /bin/sleep 60";
		std::array<char *, 4> shell_args = {const_cast<char *>("/bin/sh"), const_cast<char *>("-c"),
		                                    const_cast<char *>(command.c_str()), nullptr};
		std::array<char *, 1> empty_env  = {nullptr};
		const int spawn_result = posix_spawn(request.child_pid, "/bin/sh", request.actions, nullptr,
		                                     shell_args.data(), empty_env.data());
		const int restore_result = dup2(saved_stdin, STDIN_FILENO);
		const int restore_error  = errno;
		(void)close(saved_stdin);
		if (spawn_result != 0) {
			return spawn_result;
		}
		stalled.spawned_pid = *request.child_pid;
		if (restore_result < 0) {
			terminate_and_reap_test_child(*request.child_pid);
			return restore_error;
		}
		(void)close(stalled.ready_pipe[1]);
		stalled.ready_pipe[1] = -1;
		if (!wait_for_ready_byte(stalled.ready_pipe[0], "stalled auth helper")) {
			terminate_and_reap_test_child(*request.child_pid);
			return EIO;
		}
		return 0;
	}

	auto stalled_spawn_operations(StalledSpawnContext *context)
	    -> howdy::pam::auth_helper_process::Operations {
		auto operations             = howdy::pam::auth_helper_process::production_operations();
		operations.context          = context;
		operations.pipe2            = real_pipe2;
		operations.duplicate_fd     = integration_duplicate_fd;
		operations.actions_init     = integration_actions_init;
		operations.actions_adddup2  = integration_actions_adddup2;
		operations.actions_addclose = integration_actions_addclose;
		operations.actions_destroy  = integration_actions_destroy;
		operations.spawn            = stalled_spawn;
		operations.close            = integration_close;
		operations.read_bounded     = nullptr;
		operations.log_observer     = stalled_spawn_log;
		return operations;
	}

	auto helper_child_reaped(pid_t child_pid, std::string_view name) -> bool {
		errno                   = 0;
		const pid_t wait_result = waitpid(child_pid, nullptr, WNOHANG);
		return expect(wait_result == -1 && errno == ECHILD, std::string(name) + " reaps child");
	}

	void terminate_and_reap_test_child(pid_t child_pid) {
		if (child_pid <= 0) {
			return;
		}

		(void)kill(child_pid, SIGTERM);
		for (int attempts = 0; attempts < 50; ++attempts) {
			const pid_t wait_result = waitpid(child_pid, nullptr, WNOHANG);
			if (wait_result == child_pid || (wait_result < 0 && errno == ECHILD)) {
				return;
			}
			if (wait_result < 0 && errno != EINTR) {
				break;
			}
			usleep(1000);
		}

		(void)kill(child_pid, SIGKILL);
		while (waitpid(child_pid, nullptr, 0) < 0) {
			if (errno == ECHILD) {
				return;
			}
			if (errno != EINTR) {
				return;
			}
		}
	}

	auto wait_for_ready_byte(int ready_fd, std::string_view name) -> bool {
		char ready = 0;
		while (read(ready_fd, &ready, 1) < 0) {
			if (errno != EINTR) {
				return expect(false, std::string(name) + " reads readiness byte");
			}
		}
		return expect(ready == 'R', std::string(name) + " receives readiness byte");
	}

	auto run_deadline_output_case(std::string_view name, std::string_view output, bool close_output,
	                              bool exit_success, bool ignore_sigterm) -> bool {
		std::array<int, 2> output_pipe{};
		std::array<int, 2> ready_pipe{};
		if (!expect(pipe2(output_pipe.data(), O_CLOEXEC) == 0,
		            std::string(name) + " creates output pipe")) {
			return false;
		}
		if (pipe2(ready_pipe.data(), O_CLOEXEC) != 0) {
			(void)close(output_pipe[0]);
			(void)close(output_pipe[1]);
			return expect(false, std::string(name) + " creates readiness pipe");
		}

		const pid_t child_pid = fork();
		if (child_pid == 0) {
			(void)close(output_pipe[0]);
			(void)close(ready_pipe[0]);
			if (ignore_sigterm) {
				(void)signal(SIGTERM, SIG_IGN);
			}
			if (!output.empty() && write(output_pipe[1], output.data(), output.size()) !=
			                           static_cast<ssize_t>(output.size())) {
				_exit(EXIT_FAILURE);
			}
			if (close_output) {
				(void)close(output_pipe[1]);
			}
			if (write(ready_pipe[1], "R", 1) != 1) {
				_exit(EXIT_FAILURE);
			}
			(void)close(ready_pipe[1]);
			if (exit_success) {
				_exit(EXIT_SUCCESS);
			}
			for (;;) {
				pause();
			}
		}
		(void)close(output_pipe[1]);
		(void)close(ready_pipe[1]);
		if (!expect(child_pid > 0, std::string(name) + " forks child") ||
		    !wait_for_ready_byte(ready_pipe[0], name)) {
			(void)close(output_pipe[0]);
			(void)close(ready_pipe[0]);
			terminate_and_reap_test_child(child_pid);
			return false;
		}
		(void)close(ready_pipe[0]);

		AuthHelperSpawnFake fake;
		auto                operations = howdy::pam::auth_helper_process::production_operations();
		operations.context             = &fake;
		operations.read_bounded        = nullptr;
		operations.log_observer        = fake_auth_helper_spawn_log;
		std::string helper_output      = "stale";
		const auto  start              = std::chrono::steady_clock::now();
		const auto  timeout            = std::chrono::milliseconds(150);
		const bool  result             = howdy::pam::auth_helper_process::read_output(
		    {.child_pid = child_pid, .output_fd = output_pipe[0]}, &helper_output, operations,
		    start + timeout);
		const auto elapsed = std::chrono::steady_clock::now() - start;
		(void)close(output_pipe[0]);

		bool ok = true;
		ok &= expect(result == exit_success, std::string(name) + " returns expected result");
		if (exit_success) {
			ok &= expect(helper_output == output, std::string(name) + " preserves exact output");
		} else {
			ok &= expect(helper_output.empty(), std::string(name) + " discards failed output");
			ok &= expect(elapsed >= timeout, std::string(name) + " honors deadline");
			ok &= expect(elapsed < std::chrono::seconds(2), std::string(name) + " remains bounded");
			ok &= expect(
			    std::ranges::find(fake.log_messages, "Howdy auth helper prepare timed out") !=
			        fake.log_messages.end(),
			    std::string(name) + " logs prepare timeout");
		}
		ok &= helper_child_reaped(child_pid, name);
		return ok;
	}

	enum class ChildOutcome : std::uint8_t {
		kSuccess,
		kNonzero,
		kSignal,
	};

	auto run_combined_output_failure_case(std::string_view name, ChildOutcome outcome,
	                                      std::string_view output) -> bool {
		std::array<int, 2> output_pipe{};
		if (!expect(pipe2(output_pipe.data(), O_CLOEXEC) == 0,
		            std::string(name) + " creates output pipe")) {
			return false;
		}

		const pid_t child_pid = fork();
		if (child_pid < 0) {
			(void)close(output_pipe[0]);
			(void)close(output_pipe[1]);
			return expect(false, std::string(name) + " forks child");
		}
		if (child_pid == 0) {
			(void)close(output_pipe[0]);
			if (write(output_pipe[1], output.data(), output.size()) !=
			    static_cast<ssize_t>(output.size())) {
				_exit(EXIT_FAILURE);
			}
			(void)close(output_pipe[1]);
			switch (outcome) {
				case ChildOutcome::kSuccess:
					_exit(EXIT_SUCCESS);
				case ChildOutcome::kNonzero:
					_exit(EXIT_FAILURE);
				case ChildOutcome::kSignal:
					(void)signal(SIGTERM, SIG_DFL);
					raise(SIGTERM);
					_exit(EXIT_FAILURE);
			}
			_exit(EXIT_FAILURE);
		}

		(void)close(output_pipe[1]);
		AuthHelperSpawnFake fake;
		auto                operations = howdy::pam::auth_helper_process::production_operations();
		operations.context             = &fake;
		operations.read_bounded        = nullptr;
		operations.log_observer        = fake_auth_helper_spawn_log;
		std::string helper_output      = "stale";
		const bool  result             = howdy::pam::auth_helper_process::read_output(
		    {.child_pid = child_pid, .output_fd = output_pipe[0]}, &helper_output, operations,
		    std::chrono::steady_clock::now() + std::chrono::seconds(1));
		(void)close(output_pipe[0]);

		return expect(!result, std::string(name) + " rejects combined child failure") &&
		       expect(helper_output.empty(), std::string(name) + " clears caller-visible output") &&
		       expect(fake.log_messages.empty(), std::string(name) + " does not time out") &&
		       helper_child_reaped(child_pid, name);
	}

	auto test_auth_helper_combined_failures() -> bool {
		const std::string valid_output = "CONFIG_PATH=/run/howdy/combined/config.ini\n"
		                                 "USER_MODELS_DIR=/run/howdy/combined/models\n";
		bool              ok           = true;
		ok &= run_combined_output_failure_case("valid output and nonzero helper",
		                                       ChildOutcome::kNonzero, valid_output);
		ok &= run_combined_output_failure_case("malformed output and successful helper",
		                                       ChildOutcome::kSuccess, "MALFORMED_OUTPUT\n");
		ok &= run_combined_output_failure_case("valid output and signaled helper",
		                                       ChildOutcome::kSignal, valid_output);
		return ok;
	}

	auto run_cleanup_failure_case(std::string_view name, ChildOutcome outcome) -> bool {
		const pid_t child_pid = fork();
		if (child_pid < 0) {
			return expect(false, std::string(name) + " forks child");
		}
		if (child_pid == 0) {
			if (outcome == ChildOutcome::kSignal) {
				(void)signal(SIGTERM, SIG_DFL);
				raise(SIGTERM);
				_exit(EXIT_FAILURE);
			}
			_exit(EXIT_FAILURE);
		}

		AuthHelperSpawnFake fake;
		auto                operations = howdy::pam::auth_helper_process::production_operations();
		operations.context             = &fake;
		operations.log_observer        = fake_auth_helper_spawn_log;
		const bool result              = howdy::pam::auth_helper_process::wait_for_cleanup_helper(
		    child_pid, operations, std::chrono::steady_clock::now() + std::chrono::seconds(1));
		return expect(!result, std::string(name) + " reports child failure") &&
		       expect(std::ranges::find(fake.log_messages, "Howdy auth helper cleanup failed") !=
		                  fake.log_messages.end(),
		              std::string(name) + " records child failure") &&
		       expect(std::ranges::find(fake.log_messages, "Howdy auth helper cleanup timed out") ==
		                  fake.log_messages.end(),
		              std::string(name) + " does not time out") &&
		       helper_child_reaped(child_pid, name);
	}

	auto test_cleanup_helper_combined_failures() -> bool {
		return run_cleanup_failure_case("nonzero cleanup helper", ChildOutcome::kNonzero) &&
		       run_cleanup_failure_case("signaled cleanup helper", ChildOutcome::kSignal);
	}

	auto test_late_helper_exit_is_timed_out_after_reap() -> bool {
		const pid_t child_pid = fork();
		if (child_pid == 0) {
			_exit(EXIT_SUCCESS);
		}
		if (!expect(child_pid > 0, "late helper exit forks child")) {
			return false;
		}
		siginfo_t child_info{};
		while (waitid(P_PID, static_cast<id_t>(child_pid), &child_info, WEXITED | WNOWAIT) != 0) {
			if (errno != EINTR) {
				terminate_and_reap_test_child(child_pid);
				return expect(false, "late helper exit synchronizes exited child");
			}
		}

		AuthHelperSpawnFake fake;
		auto                operations = howdy::pam::auth_helper_process::production_operations();
		operations.context             = &fake;
		operations.log_observer        = fake_auth_helper_spawn_log;
		const bool result              = howdy::pam::auth_helper_process::wait_for_cleanup_helper(
		    child_pid, operations, std::chrono::steady_clock::now());
		return expect(!result, "late helper exit is timed out") &&
		       expect(std::ranges::find(fake.log_messages, "Howdy auth helper cleanup timed out") !=
		                  fake.log_messages.end(),
		              "late helper exit logs timeout") &&
		       helper_child_reaped(child_pid, "late helper exit");
	}

	auto test_prepare_runtime_auth_files_stalled_child() -> bool {
		StalledSpawnContext context;
		if (!expect(pipe(context.ready_pipe.data()) == 0,
		            "prepare orchestration creates ready pipe")) {
			return false;
		}
		howdy::pam::PreparedRuntimeFiles prepared;
		const auto                       start      = std::chrono::steady_clock::now();
		const auto                       timeout    = std::chrono::milliseconds(150);
		const auto                       operations = stalled_spawn_operations(&context);
		const bool result = howdy::pam::auth_helper_process::prepare_runtime_auth_files(
		    "alice", &prepared, operations, start + timeout);
		const auto elapsed = std::chrono::steady_clock::now() - start;
		(void)close(context.ready_pipe[0]);
		(void)close(context.ready_pipe[1]);
		return expect(!result, "prepare orchestration rejects stalled child") &&
		       expect(context.spawn_calls == 1, "prepare orchestration spawns once") &&
		       expect(elapsed >= timeout, "prepare orchestration honors deadline") &&
		       expect(elapsed < std::chrono::seconds(2), "prepare orchestration remains bounded") &&
		       expect(
		           std::ranges::find(context.log_messages, "Howdy auth helper prepare timed out") !=
		               context.log_messages.end(),
		           "prepare orchestration logs timeout") &&
		       helper_child_reaped(context.spawned_pid, "prepare orchestration");
	}

	auto test_cleanup_runtime_auth_files_stalled_child() -> bool {
		StalledSpawnContext context;
		if (!expect(pipe(context.ready_pipe.data()) == 0,
		            "cleanup orchestration creates ready pipe")) {
			return false;
		}
		const auto start      = std::chrono::steady_clock::now();
		const auto timeout    = std::chrono::milliseconds(150);
		const auto operations = stalled_spawn_operations(&context);
		howdy::pam::auth_helper_process::cleanup_runtime_auth_files(kStagedRoot, operations,
		                                                            start + timeout);
		const auto elapsed = std::chrono::steady_clock::now() - start;
		(void)close(context.ready_pipe[0]);
		(void)close(context.ready_pipe[1]);
		return expect(context.spawn_calls == 1, "cleanup orchestration spawns once") &&
		       expect(elapsed >= timeout, "cleanup orchestration honors deadline") &&
		       expect(elapsed < std::chrono::seconds(2), "cleanup orchestration remains bounded") &&
		       expect(
		           std::ranges::find(context.log_messages, "Howdy auth helper cleanup timed out") !=
		               context.log_messages.end(),
		           "cleanup orchestration logs timeout") &&
		       helper_child_reaped(context.spawned_pid, "cleanup orchestration");
	}

	auto test_auth_helper_absolute_deadlines() -> bool {
		const std::string valid_output = "CONFIG_PATH=/run/howdy/deadline/config.ini\n"
		                                 "USER_MODELS_DIR=/run/howdy/deadline/models\n";
		bool              ok           = true;
		ok &= run_deadline_output_case("normal helper", valid_output, true, true, false);
		ok &= run_deadline_output_case("silent open pipe", "", false, false, false);
		ok &= run_deadline_output_case("partial stalled output", "CONFIG_PATH=/partial", false,
		                               false, false);
		ok &= run_deadline_output_case("closed pipe live child", valid_output, true, false, false);
		ok &= run_deadline_output_case("SIGTERM-ignoring helper", valid_output, true, false, true);
		return ok;
	}

	auto test_auth_helper_output_limit() -> bool {
		std::array<int, 2> output_pipe{};
		std::array<int, 2> ready_pipe{};
		if (!expect(pipe2(output_pipe.data(), O_CLOEXEC) == 0,
		            "output limit creates output pipe")) {
			return false;
		}
		if (pipe2(ready_pipe.data(), O_CLOEXEC) != 0) {
			(void)close(output_pipe[0]);
			(void)close(output_pipe[1]);
			return expect(false, "output limit creates readiness pipe");
		}

		const pid_t child_pid = fork();
		if (child_pid == 0) {
			(void)close(output_pipe[0]);
			(void)close(ready_pipe[0]);
			if (write(ready_pipe[1], "R", 1) != 1) {
				_exit(EXIT_FAILURE);
			}
			(void)close(ready_pipe[1]);
			const std::string output(howdy::pam::auth_helper_process::output_limit(), 'h');
			const bool        wrote = write(output_pipe[1], output.data(), output.size()) ==
			                          static_cast<ssize_t>(output.size());
			(void)close(output_pipe[1]);
			_exit(wrote ? EXIT_SUCCESS : EXIT_FAILURE);
		}
		(void)close(output_pipe[1]);
		(void)close(ready_pipe[1]);
		if (!expect(child_pid > 0, "output limit forks child") ||
		    !wait_for_ready_byte(ready_pipe[0], "output limit")) {
			(void)close(output_pipe[0]);
			(void)close(ready_pipe[0]);
			terminate_and_reap_test_child(child_pid);
			return false;
		}
		(void)close(ready_pipe[0]);

		std::string output      = "stale";
		auto        operations  = howdy::pam::auth_helper_process::production_operations();
		operations.read_bounded = nullptr;
		const bool result       = howdy::pam::auth_helper_process::read_output(
		    {.child_pid = child_pid, .output_fd = output_pipe[0]}, &output, operations,
		    std::chrono::steady_clock::now() + std::chrono::seconds(1));
		(void)close(output_pipe[0]);
		return expect(!result, "output limit is rejected") &&
		       expect(output.empty(), "output limit data is discarded") &&
		       helper_child_reaped(child_pid, "output limit");
	}

	auto test_cleanup_helper_deadline() -> bool {
		std::array<int, 2> ready_pipe{};
		if (!expect(pipe2(ready_pipe.data(), O_CLOEXEC) == 0,
		            "cleanup deadline creates readiness pipe")) {
			return false;
		}
		const pid_t child_pid = fork();
		if (child_pid == 0) {
			(void)close(ready_pipe[0]);
			(void)signal(SIGTERM, SIG_IGN);
			if (write(ready_pipe[1], "R", 1) != 1) {
				_exit(EXIT_FAILURE);
			}
			(void)close(ready_pipe[1]);
			for (;;) {
				pause();
			}
		}
		(void)close(ready_pipe[1]);
		if (!expect(child_pid > 0, "cleanup deadline forks child") ||
		    !wait_for_ready_byte(ready_pipe[0], "cleanup deadline")) {
			(void)close(ready_pipe[0]);
			terminate_and_reap_test_child(child_pid);
			return false;
		}
		(void)close(ready_pipe[0]);

		AuthHelperSpawnFake fake;
		auto                operations = howdy::pam::auth_helper_process::production_operations();
		operations.context             = &fake;
		operations.log_observer        = fake_auth_helper_spawn_log;
		const auto start               = std::chrono::steady_clock::now();
		const auto timeout             = std::chrono::milliseconds(150);
		const bool result              = howdy::pam::auth_helper_process::wait_for_cleanup_helper(
		    child_pid, operations, start + timeout);
		const auto elapsed = std::chrono::steady_clock::now() - start;
		return expect(!result, "cleanup deadline reports failure") &&
		       expect(elapsed >= timeout, "cleanup deadline honors deadline") &&
		       expect(elapsed < std::chrono::seconds(2), "cleanup deadline remains bounded") &&
		       expect(std::ranges::find(fake.log_messages, "Howdy auth helper cleanup timed out") !=
		                  fake.log_messages.end(),
		              "cleanup deadline logs cleanup timeout") &&
		       helper_child_reaped(child_pid, "cleanup deadline");
	}
}  // namespace

auto run_runtime_session_deadline_tests() -> bool {
	bool ok = true;
	ok &= test_auth_helper_absolute_deadlines();
	ok &= test_auth_helper_combined_failures();
	ok &= test_cleanup_helper_combined_failures();
	ok &= test_late_helper_exit_is_timed_out_after_reap();
	ok &= test_prepare_runtime_auth_files_stalled_child();
	ok &= test_cleanup_runtime_auth_files_stalled_child();
	ok &= test_auth_helper_output_limit();
	ok &= test_cleanup_helper_deadline();
	return ok;
}
