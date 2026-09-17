#include "runtime/runtime_session_process_test_support.hpp"
#include "runtime/runtime_session_test_groups.hpp"
#include "support/process_test_support.hpp"

namespace {
	using namespace howdy::test::runtime_session;
	using howdy::test::process::ScopedSignalBlock;

	auto IntegrationDuplicateFd(void *context, int fd, int minimum_fd) -> int {
		(void)context;
		return fcntl(fd, F_DUPFD_CLOEXEC, minimum_fd);
	}

	auto IntegrationActionsInit(void *context, posix_spawn_file_actions_t *actions) -> int {
		(void)context;
		return posix_spawn_file_actions_init(actions);
	}

	auto IntegrationActionsAdddup2(void *context, posix_spawn_file_actions_t *actions,
	                               int source_fd, int target_fd) -> int {
		(void)context;
		return posix_spawn_file_actions_adddup2(actions, source_fd, target_fd);
	}

	auto IntegrationActionsAddclose(void *context, posix_spawn_file_actions_t *actions, int fd)
	    -> int {
		(void)context;
		return posix_spawn_file_actions_addclose(actions, fd);
	}

	auto IntegrationActionsDestroy(void *context, posix_spawn_file_actions_t *actions) -> int {
		(void)context;
		return posix_spawn_file_actions_destroy(actions);
	}

	auto IntegrationClose(void *context, int fd) -> int {
		(void)context;
		return close(fd);
	}

	auto WaitForReadyByte(int ready_fd, std::string_view name) -> bool;
	void TerminateAndReapTestChild(pid_t child_pid);

	auto RealPipe2(void *context, int *pipe_fds, int flags) -> int {
		(void)context;
		return pipe2(pipe_fds, flags);
	}

	struct StalledSpawnContext {
		std::array<int, 2>       ready_pipe = {-1, -1};
		std::vector<std::string> log_messages;
		pid_t                    spawned_pid = -1;
		int                      spawn_calls = 0;
	};

	void StalledSpawnLog(void *context, std::string_view message) {
		static_cast<StalledSpawnContext *>(context)->log_messages.emplace_back(message);
	}

	auto StalledSpawn(const howdy::pam::auth_helper_process::SpawnRequest &request) -> int {
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
		std::array<char *, 3> helper_args = {const_cast<char *>("pam_runtime_session_test"),
		                                     const_cast<char *>("--stalled-helper"), nullptr};
		std::array<char *, 1> empty_env   = {nullptr};
		const int spawn_result   = posix_spawn(request.child_pid, "/proc/self/exe", request.actions,
		                                       nullptr, helper_args.data(), empty_env.data());
		const int restore_result = dup2(saved_stdin, STDIN_FILENO);
		const int restore_error  = errno;
		(void)close(saved_stdin);
		if (spawn_result != 0) {
			return spawn_result;
		}
		stalled.spawned_pid = *request.child_pid;
		if (restore_result < 0) {
			TerminateAndReapTestChild(*request.child_pid);
			return restore_error;
		}
		(void)close(stalled.ready_pipe[1]);
		stalled.ready_pipe[1] = -1;
		if (!WaitForReadyByte(stalled.ready_pipe[0], "stalled auth helper")) {
			TerminateAndReapTestChild(*request.child_pid);
			return EIO;
		}
		return 0;
	}

	auto StalledSpawnOperations(StalledSpawnContext *context)
	    -> howdy::pam::auth_helper_process::Operations {
		auto operations             = howdy::pam::auth_helper_process::ProductionOperations();
		operations.context          = context;
		operations.pipe2            = RealPipe2;
		operations.duplicate_fd     = IntegrationDuplicateFd;
		operations.actions_init     = IntegrationActionsInit;
		operations.actions_adddup2  = IntegrationActionsAdddup2;
		operations.actions_addclose = IntegrationActionsAddclose;
		operations.actions_destroy  = IntegrationActionsDestroy;
		operations.spawn            = StalledSpawn;
		operations.close            = IntegrationClose;
		operations.read_bounded     = nullptr;
		operations.log_observer     = StalledSpawnLog;
		return operations;
	}

	auto HelperChildReaped(pid_t child_pid, std::string_view name) -> bool {
		errno                   = 0;
		const pid_t wait_result = waitpid(child_pid, nullptr, WNOHANG);
		return Expect(wait_result == -1 && errno == ECHILD, std::string(name) + " reaps child");
	}

	void TerminateAndReapTestChild(pid_t child_pid) {
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

	auto WaitForReadyByte(int ready_fd, std::string_view name) -> bool {
		char ready = 0;
		while (read(ready_fd, &ready, 1) < 0) {
			if (errno != EINTR) {
				return Expect(false, std::string(name) + " reads readiness byte");
			}
		}
		return Expect(ready == 'R', std::string(name) + " receives readiness byte");
	}

	auto RunDeadlineOutputCase(std::string_view name, std::string_view output, bool close_output,
	                           bool exit_success, bool ignore_sigterm) -> bool {
		std::array<int, 2> output_pipe{};
		std::array<int, 2> ready_pipe{};
		if (!Expect(pipe2(output_pipe.data(), O_CLOEXEC) == 0,
		            std::string(name) + " creates output pipe")) {
			return false;
		}
		if (pipe2(ready_pipe.data(), O_CLOEXEC) != 0) {
			(void)close(output_pipe[0]);
			(void)close(output_pipe[1]);
			return Expect(false, std::string(name) + " creates readiness pipe");
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
		if (!Expect(child_pid > 0, std::string(name) + " forks child") ||
		    !WaitForReadyByte(ready_pipe[0], name)) {
			(void)close(output_pipe[0]);
			(void)close(ready_pipe[0]);
			TerminateAndReapTestChild(child_pid);
			return false;
		}
		(void)close(ready_pipe[0]);

		AuthHelperSpawnFake fake;
		auto                operations = howdy::pam::auth_helper_process::ProductionOperations();
		operations.context             = &fake;
		operations.read_bounded        = nullptr;
		operations.log_observer        = FakeAuthHelperSpawnLog;
		std::string helper_output      = "stale";
		const auto  start              = std::chrono::steady_clock::now();
		const auto  timeout            = std::chrono::milliseconds(150);
		const bool  result             = howdy::pam::auth_helper_process::ReadOutput(
		    {.child_pid = child_pid, .output_fd = output_pipe[0]}, &helper_output, operations,
		    start + timeout);
		const auto elapsed = std::chrono::steady_clock::now() - start;
		(void)close(output_pipe[0]);

		bool ok = true;
		ok &= Expect(result == exit_success, std::string(name) + " returns expected result");
		if (exit_success) {
			ok &= Expect(helper_output == output, std::string(name) + " preserves exact output");
		} else {
			ok &= Expect(helper_output.empty(), std::string(name) + " discards failed output");
			ok &= Expect(elapsed >= timeout, std::string(name) + " honors deadline");
			ok &= Expect(elapsed < std::chrono::seconds(2), std::string(name) + " remains bounded");
			ok &= Expect(
			    std::ranges::find(fake.log_messages, "Howdy auth helper prepare timed out") !=
			        fake.log_messages.end(),
			    std::string(name) + " logs prepare timeout");
		}
		ok &= HelperChildReaped(child_pid, name);
		return ok;
	}

	enum class ChildOutcome : std::uint8_t {
		kSuccess,
		kNonzero,
		kSignal,
	};

	auto RunCombinedOutputFailureCase(std::string_view name, ChildOutcome outcome,
	                                  std::string_view output) -> bool {
		std::array<int, 2> output_pipe{};
		if (!Expect(pipe2(output_pipe.data(), O_CLOEXEC) == 0,
		            std::string(name) + " creates output pipe")) {
			return false;
		}

		const pid_t child_pid = fork();
		if (child_pid < 0) {
			(void)close(output_pipe[0]);
			(void)close(output_pipe[1]);
			return Expect(false, std::string(name) + " forks child");
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
		auto                operations = howdy::pam::auth_helper_process::ProductionOperations();
		operations.context             = &fake;
		operations.read_bounded        = nullptr;
		operations.log_observer        = FakeAuthHelperSpawnLog;
		std::string helper_output      = "stale";
		const bool  result             = howdy::pam::auth_helper_process::ReadOutput(
		    {.child_pid = child_pid, .output_fd = output_pipe[0]}, &helper_output, operations,
		    std::chrono::steady_clock::now() + std::chrono::seconds(1));
		(void)close(output_pipe[0]);

		return Expect(!result, std::string(name) + " rejects combined child failure") &&
		       Expect(helper_output.empty(), std::string(name) + " clears caller-visible output") &&
		       Expect(fake.log_messages.empty(), std::string(name) + " does not time out") &&
		       HelperChildReaped(child_pid, name);
	}

	auto TestAuthHelperCombinedFailures() -> bool {
		const std::string valid_output = TestPreparedRuntimeOutput("comb01");
		bool              ok           = true;
		ok &= RunCombinedOutputFailureCase("valid output and nonzero helper",
		                                   ChildOutcome::kNonzero, valid_output);
		ok &= RunCombinedOutputFailureCase("malformed output and successful helper",
		                                   ChildOutcome::kSuccess, "MALFORMED_OUTPUT\n");
		ok &= RunCombinedOutputFailureCase("valid output and signaled helper",
		                                   ChildOutcome::kSignal, valid_output);
		return ok;
	}

	auto TestPrepareRuntimeAuthFilesStalledChild() -> bool {
		StalledSpawnContext context;
		if (!Expect(pipe(context.ready_pipe.data()) == 0,
		            "prepare orchestration creates ready pipe")) {
			return false;
		}
		howdy::pam::PreparedRuntimeFiles prepared;
		const auto                       start      = std::chrono::steady_clock::now();
		const auto                       timeout    = std::chrono::milliseconds(150);
		const auto                       operations = StalledSpawnOperations(&context);
		const bool result = howdy::pam::auth_helper_process::PrepareRuntimeAuthFiles(
		    "alice", &prepared, operations, start + timeout);
		const auto elapsed = std::chrono::steady_clock::now() - start;
		(void)close(context.ready_pipe[0]);
		(void)close(context.ready_pipe[1]);
		return Expect(!result, "prepare orchestration rejects stalled child") &&
		       Expect(context.spawn_calls == 1, "prepare orchestration spawns once") &&
		       Expect(elapsed >= timeout, "prepare orchestration honors deadline") &&
		       Expect(elapsed < std::chrono::seconds(2), "prepare orchestration remains bounded") &&
		       Expect(
		           std::ranges::find(context.log_messages, "Howdy auth helper prepare timed out") !=
		               context.log_messages.end(),
		           "prepare orchestration logs timeout") &&
		       HelperChildReaped(context.spawned_pid, "prepare orchestration");
	}

	auto TestAuthHelperAbsoluteDeadlines() -> bool {
		const std::string valid_output = TestPreparedRuntimeOutput("dead01");
		bool              ok           = true;
		ok &= RunDeadlineOutputCase("normal helper", valid_output, true, true, false);
		ok &= RunDeadlineOutputCase("silent open pipe", "", false, false, false);
		ok &= RunDeadlineOutputCase("partial stalled output", "CONFIG_PATH=/partial", false, false,
		                            false);
		ok &= RunDeadlineOutputCase("closed pipe live child", valid_output, true, false, false);
		ok &= RunDeadlineOutputCase("SIGTERM-ignoring helper", valid_output, true, false, true);
		return ok;
	}

	auto TestAuthHelperOutputLimit() -> bool {
		std::array<int, 2> output_pipe{};
		std::array<int, 2> ready_pipe{};
		if (!Expect(pipe2(output_pipe.data(), O_CLOEXEC) == 0,
		            "output limit creates output pipe")) {
			return false;
		}
		if (pipe2(ready_pipe.data(), O_CLOEXEC) != 0) {
			(void)close(output_pipe[0]);
			(void)close(output_pipe[1]);
			return Expect(false, "output limit creates readiness pipe");
		}

		const pid_t child_pid = fork();
		if (child_pid == 0) {
			(void)close(output_pipe[0]);
			(void)close(ready_pipe[0]);
			if (write(ready_pipe[1], "R", 1) != 1) {
				_exit(EXIT_FAILURE);
			}
			(void)close(ready_pipe[1]);
			const std::string output(howdy::pam::auth_helper_process::OutputLimit(), 'h');
			const bool        wrote = write(output_pipe[1], output.data(), output.size()) ==
			                          static_cast<ssize_t>(output.size());
			(void)close(output_pipe[1]);
			_exit(wrote ? EXIT_SUCCESS : EXIT_FAILURE);
		}
		(void)close(output_pipe[1]);
		(void)close(ready_pipe[1]);
		if (!Expect(child_pid > 0, "output limit forks child") ||
		    !WaitForReadyByte(ready_pipe[0], "output limit")) {
			(void)close(output_pipe[0]);
			(void)close(ready_pipe[0]);
			TerminateAndReapTestChild(child_pid);
			return false;
		}
		(void)close(ready_pipe[0]);

		std::string output      = "stale";
		auto        operations  = howdy::pam::auth_helper_process::ProductionOperations();
		operations.read_bounded = nullptr;
		const bool result       = howdy::pam::auth_helper_process::ReadOutput(
		    {.child_pid = child_pid, .output_fd = output_pipe[0]}, &output, operations,
		    std::chrono::steady_clock::now() + std::chrono::seconds(1));
		(void)close(output_pipe[0]);
		return Expect(!result, "output limit is rejected") &&
		       Expect(output.empty(), "output limit data is discarded") &&
		       HelperChildReaped(child_pid, "output limit");
	}

	auto TestAuthHelperBlockedSignalTimeout() -> bool {
		const std::string valid_output = TestPreparedRuntimeOutput("mask01");
		bool              ok           = true;
		{
			ScopedSignalBlock blocked_sigchld(SIGCHLD);
			if (!Expect(blocked_sigchld.Valid(), "blocked SIGCHLD auth-helper guard installs")) {
				return false;
			}
			ok &= RunDeadlineOutputCase("blocked SIGCHLD auth-helper timeout", valid_output, true,
			                            false, false);
		}
		{
			ScopedSignalBlock blocked_sigterm(SIGTERM);
			if (!Expect(blocked_sigterm.Valid(), "blocked SIGTERM auth-helper guard installs")) {
				return false;
			}
			ok &= RunDeadlineOutputCase("blocked SIGTERM auth-helper timeout", valid_output, true,
			                            false, false);
		}
		return ok;
	}
}  // namespace

auto RunRuntimeSessionDeadlineTests() -> bool {
	bool ok = true;
	ok &= TestAuthHelperAbsoluteDeadlines();
	ok &= TestAuthHelperCombinedFailures();
	ok &= TestPrepareRuntimeAuthFilesStalledChild();
	ok &= TestAuthHelperOutputLimit();
	ok &= TestAuthHelperBlockedSignalTimeout();
	return ok;
}
