#include "../../src/runtime/auth_helper_process/internal.hpp"
#include "runtime/auth_helper_process.hpp"
#include "runtime/runtime_session.hpp"
#include "runtime/runtime_session_process_test_support.hpp"
#include "runtime/runtime_session_test_groups.hpp"
#include "support/fd_io.hpp"
#include "support/process_test_support.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>  // IWYU pragma: keep
#include <sys/wait.h>

namespace {
	using howdy::test::Expect;
	using howdy::test::process::ScopedSignalBlock;
	using howdy::test::runtime_session::TestPreparedRuntimeOutput;

	auto WaitForReadyByte(int ready_fd, std::string_view name) -> bool;
	void TerminateAndReapTestChild(pid_t child_pid);

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
		if (dup2(stalled.ready_pipe.at(1), STDIN_FILENO) < 0) {
			const int duplicate_error = errno;
			(void)close(saved_stdin);
			return duplicate_error;
		}
		std::string           program("pam_runtime_session_test");
		std::string           mode("--stalled-helper");
		std::array<char *, 3> helper_args = {program.data(), mode.data(), nullptr};
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
		(void)close(stalled.ready_pipe.at(1));
		stalled.ready_pipe.at(1) = -1;
		if (!WaitForReadyByte(stalled.ready_pipe.at(0), "stalled auth helper")) {
			TerminateAndReapTestChild(*request.child_pid);
			return EIO;
		}
		return 0;
	}

	auto StalledSpawnOperations(StalledSpawnContext *context)
	    -> howdy::pam::auth_helper_process::Operations {
		auto operations         = howdy::pam::auth_helper_process::ProductionOperations();
		operations.context      = context;
		operations.spawn        = StalledSpawn;
		operations.log_observer = StalledSpawnLog;
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

	enum class ChildOutcome : std::uint8_t {
		kSuccess,
		kNonzero,
		kSignal,
		kStall
	};

	struct PrepareCase {
		std::array<int, 2>       output_pipe  = {-1, -1};
		std::array<int, 2>       lease_socket = {-1, -1};
		std::string              output;
		ChildOutcome             outcome        = ChildOutcome::kSuccess;
		bool                     close_output   = true;
		bool                     ignore_sigterm = false;
		pid_t                    child_pid      = -1;
		std::vector<std::string> log_messages;
	};

	struct ReadFailure {
		int fd                 = -1;
		int reads_before_error = -1;
		int successful_reads   = 0;
	};

	auto InjectedReadFailure() -> ReadFailure & {
		static ReadFailure failure;
		return failure;
	}

	auto CasePipe(void *context, int *fds, int flags) -> int {
		const int result = pipe2(fds, flags);
		if (result == 0) {
			auto      &state         = *static_cast<PrepareCase *>(context);
			const auto pair          = std::span(fds, std::size_t{2});
			state.output_pipe        = {pair.front(), pair.back()};
			InjectedReadFailure().fd = pair.front();
		}
		return result;
	}

	auto CaseSocket(void *context, int domain, int type, int protocol, int *fds) -> int {
		const int result = socketpair(domain, type, protocol, fds);
		if (result == 0) {
			const auto pair                                   = std::span(fds, std::size_t{2});
			static_cast<PrepareCase *>(context)->lease_socket = {pair.front(), pair.back()};
		}
		return result;
	}

	auto CaseSpawn(const howdy::pam::auth_helper_process::SpawnRequest &request) -> int {
		auto       &state = *static_cast<PrepareCase *>(request.context);
		const pid_t pid   = fork();
		if (pid < 0) {
			return errno;
		}
		if (pid == 0) {
			(void)alarm(5);
			(void)close(state.output_pipe.at(0));
			(void)close(state.lease_socket.at(0));
			if (state.ignore_sigterm) {
				(void)signal(SIGTERM, SIG_IGN);
			}
			char  marker = 'L';
			iovec data{.iov_base = &marker, .iov_len = 1};
			alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(int))> control{};
			msghdr                                                     message{};
			message.msg_iov        = &data;
			message.msg_iovlen     = 1;
			message.msg_control    = control.data();
			message.msg_controllen = control.size();
			auto *header           = CMSG_FIRSTHDR(&message);
			header->cmsg_level     = SOL_SOCKET;
			header->cmsg_type      = SCM_RIGHTS;
			header->cmsg_len       = CMSG_LEN(sizeof(int));
			const int descriptor   = open("/dev/null", O_RDONLY | O_CLOEXEC);
			std::memcpy(CMSG_DATA(header), &descriptor, sizeof(descriptor));
			if (sendmsg(state.lease_socket.at(1), &message, MSG_NOSIGNAL) != 1 ||
			    !howdy::native::WriteAllToFd(state.output_pipe.at(1), state.output)) {
				_exit(EXIT_FAILURE);
			}
			if (state.close_output) {
				(void)close(state.output_pipe.at(1));
			}
			(void)close(state.lease_socket.at(1));
			switch (state.outcome) {
				case ChildOutcome::kSuccess:
					_exit(EXIT_SUCCESS);
				case ChildOutcome::kNonzero:
					_exit(EXIT_FAILURE);
				case ChildOutcome::kSignal:
					(void)signal(SIGTERM, SIG_DFL);
					(void)raise(SIGTERM);
					_exit(EXIT_FAILURE);
				case ChildOutcome::kStall:
					for (;;) {
						pause();
					}
			}
		}
		state.child_pid    = pid;
		*request.child_pid = pid;
		return 0;
	}

	auto RunPrepareCase(std::string_view name, PrepareCase state, bool timeout,
	                    bool reaches_lease_validation = false, int reads_before_error = -1)
	    -> bool {
		InjectedReadFailure()   = {.reads_before_error = reads_before_error};
		auto operations         = howdy::pam::auth_helper_process::ProductionOperations();
		operations.context      = &state;
		operations.pipe2        = CasePipe;
		operations.socketpair   = CaseSocket;
		operations.spawn        = CaseSpawn;
		operations.log_observer = [](void *context, std::string_view message) -> void {
			static_cast<PrepareCase *>(context)->log_messages.emplace_back(message);
		};
		howdy::pam::PreparedRuntimeFiles prepared;
		const auto                       start = std::chrono::steady_clock::now();
		const auto                       duration =
		    timeout ? std::chrono::milliseconds(150) : std::chrono::milliseconds(1000);
		const bool result = howdy::pam::auth_helper_process::PrepareRuntimeAuthFiles(
		    "alice", &prepared, operations, start + duration);
		const auto elapsed          = std::chrono::steady_clock::now() - start;
		const int  successful_reads = InjectedReadFailure().successful_reads;
		InjectedReadFailure()       = {};
		// The transport lease is intentionally not a runtime lock. A normal response
		// must reach final descriptor validation; no test bypasses that validation.
		bool ok =
		    Expect(!result && !prepared.lease_fd.Valid(), std::string(name) + " fails closed");
		ok &= Expect(prepared.config_path.empty() != reaches_lease_validation,
		             std::string(name) + " reaches expected validation stage");
		if (reaches_lease_validation) {
			const auto parsed =
			    howdy::pam::auth_helper_process::internal::ParseAuthHelperOutput(state.output);
			ok &= Expect(prepared.config_path == parsed.config_path &&
			                 prepared.user_models_dir == parsed.user_models_dir,
			             std::string(name) + " preserves parsed paths");
		}
		const bool logged_timeout =
		    std::ranges::find(state.log_messages, "Howdy auth helper prepare timed out") !=
		    state.log_messages.end();
		ok &= Expect(logged_timeout == timeout, std::string(name) + " logs only actual timeout");
		ok &= Expect(elapsed < std::chrono::seconds(2), std::string(name) + " remains bounded");
		if (timeout) {
			ok &= Expect(elapsed >= duration, std::string(name) + " honors deadline");
		}
		if (reads_before_error > 0) {
			ok &= Expect(successful_reads == reads_before_error,
			             std::string(name) + " collects partial output before error");
		}
		ok &= HelperChildReaped(state.child_pid, name);
		return ok;
	}

	auto TestPrepareRuntimeAuthFilesStalledChild() -> bool {
		StalledSpawnContext context;
		if (!Expect(pipe2(context.ready_pipe.data(), O_CLOEXEC) == 0,
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
		(void)close(context.ready_pipe.at(0));
		(void)close(context.ready_pipe.at(1));
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

}  // namespace

extern "C" auto RealRead(int fd, void *buffer, std::size_t count) -> ssize_t __asm__("__real_read");
extern "C" auto TestRead(int fd, void *buffer, std::size_t count) -> ssize_t __asm__("__wrap_read");
extern "C" auto TestReadChecked(int fd, void *buffer, std::size_t count, std::size_t buffer_size)
    -> ssize_t __asm__("__wrap___read_chk");

extern "C" auto TestRead(int fd, void *buffer, std::size_t count) -> ssize_t {
	auto &failure = InjectedReadFailure();
	if (fd == failure.fd && failure.reads_before_error >= 0) {
		if (failure.successful_reads >= failure.reads_before_error) {
			errno = EIO;
			return -1;
		}
		const auto result = RealRead(fd, buffer, count);
		if (result > 0) {
			++failure.successful_reads;
		}
		return result;
	}
	return RealRead(fd, buffer, count);
}

// glibc fixes this argument order.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
extern "C" auto TestReadChecked(int fd, void *buffer, std::size_t count,
                                [[maybe_unused]] std::size_t buffer_size) -> ssize_t {
	return TestRead(fd, buffer, count);
}

auto RunRuntimeSessionDeadlineTests() -> bool {
	const auto output = TestPreparedRuntimeOutput("dead01");
	bool       ok     = TestPrepareRuntimeAuthFilesStalledChild();
	ok &= RunPrepareCase("normal response", {.output = output}, false, true);
	ok &= RunPrepareCase("empty output", {}, false);
	ok &= RunPrepareCase("silent open pipe",
	                     {.outcome = ChildOutcome::kStall, .close_output = false}, true);
	ok &= RunPrepareCase(
	    "partial stalled output",
	    {.output = "CONFIG_PATH=/partial", .outcome = ChildOutcome::kStall, .close_output = false},
	    true);
	ok &= RunPrepareCase("closed pipe live child",
	                     {.output = output, .outcome = ChildOutcome::kStall}, true);
	ok &= RunPrepareCase(
	    "SIGTERM-ignoring child",
	    {.output = output, .outcome = ChildOutcome::kStall, .ignore_sigterm = true}, true);
	ok &= RunPrepareCase("nonzero child", {.output = output, .outcome = ChildOutcome::kNonzero},
	                     false);
	ok &= RunPrepareCase("signaled child", {.output = output, .outcome = ChildOutcome::kSignal},
	                     false);
	for (const auto &invalid : {std::string("MALFORMED_OUTPUT\n"), output + "UNKNOWN=value\n",
	                            output + "CONFIG_PATH=/duplicate\n"}) {
		ok &= RunPrepareCase("malformed response", {.output = invalid}, false);
	}
	ok &=
	    RunPrepareCase("output limit",
		               {.output = std::string(
	                        howdy::pam::auth_helper_process::internal::kAuthHelperOutputLimit, 'h'),
		                .outcome = ChildOutcome::kStall},
		               false);
	ok &= RunPrepareCase("read error", {.output = output, .outcome = ChildOutcome::kStall}, false,
	                     false, 0);
	ok &= RunPrepareCase("partial read error", {.output = output, .outcome = ChildOutcome::kStall},
	                     false, false, 1);
	for (const int signal_number : {SIGCHLD, SIGTERM}) {
		ScopedSignalBlock blocked(signal_number);
		ok &= Expect(blocked.Valid(), "blocks helper signal");
		ok &= RunPrepareCase("blocked signal timeout",
		                     {.output = output, .outcome = ChildOutcome::kStall}, true);
	}
	return ok;
}
