#pragma once

#include "protocol/auth_helper_protocol.hpp"
#include "runtime/auth_helper_process.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <paths.hpp>
#include <spawn.h>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace howdy::test::runtime_session {
	using howdy::test::Expect;

	inline auto TestPreparedRuntimeRoot(std::string_view suffix = "helper")
	    -> std::filesystem::path {
		(void)suffix;
		return howdy::native::auth_helper_protocol::PreparedRuntimeGenerationDir(
		    howdy::native::auth_helper_protocol::PreparedRuntimeRoot(), getuid(),
		    howdy::native::auth_helper_protocol::RuntimeGenerationSlot::kSlot0);
	}

	inline auto TestPreparedRuntimeConfigPath(std::string_view suffix = "helper")
	    -> std::filesystem::path {
		return howdy::native::auth_helper_protocol::PreparedConfigPath(
		    TestPreparedRuntimeRoot(suffix));
	}

	inline auto TestPreparedRuntimeModelsDir(std::string_view suffix = "helper")
	    -> std::filesystem::path {
		return howdy::native::auth_helper_protocol::PreparedUserModelsDir(
		    TestPreparedRuntimeRoot(suffix));
	}

	inline auto TestPreparedRuntimeOutput(std::string_view suffix = "helper") -> std::string {
		return "CONFIG_PATH=" + TestPreparedRuntimeConfigPath(suffix).string() +
		       "\nUSER_MODELS_DIR=" + TestPreparedRuntimeModelsDir(suffix).string() + "\n";
	}

	struct AuthHelperSpawnFake {
		std::string                      fail_operation;
		std::vector<std::string>         operations;
		std::vector<std::pair<int, int>> duplicate_fd_requests;
		std::vector<std::pair<int, int>> dup2_fds;
		std::vector<int>                 action_close_fds;
		std::vector<int>                 action_closefrom_fds;
		std::vector<int>                 parent_close_fds;
		std::vector<int>                 pipe_flags;
		std::vector<std::array<int, 3>>  socket_parameters;
		std::vector<std::string>         spawn_argv;
		std::vector<std::string>         spawn_env;
		std::vector<std::string>         log_messages;
		std::string                      spawn_path;
		std::array<int, 2>               next_pipe_fds     = {10, 11};
		std::array<int, 2>               next_socket_fds   = {12, 13};
		int                              next_duplicate_fd = 20;
		int                              spawn_calls       = 0;
		int                              destroy_calls     = 0;
		int                              inherited_fd      = -1;
		bool                             destroy_fails     = false;
		pid_t                            spawned_pid       = -1;
	};

	inline auto FakeResult(AuthHelperSpawnFake &fake, std::string_view operation) -> int {
		fake.operations.emplace_back(operation);
		if (fake.fail_operation != operation) {
			return 0;
		}
		errno = EIO;
		return EIO;
	}

	inline auto FakePipe2(void *context, int *fds, int flags) -> int {
		auto &fake = *static_cast<AuthHelperSpawnFake *>(context);
		fake.pipe_flags.push_back(flags);
		fds[0] = fake.next_pipe_fds[0];
		fds[1] = fake.next_pipe_fds[1];
		return FakeResult(fake, "pipe2");
	}

	inline auto FakeSocketpair(void *context, int domain, int type, int protocol, int *fds) -> int {
		auto &fake = *static_cast<AuthHelperSpawnFake *>(context);
		fake.socket_parameters.push_back({domain, type, protocol});
		fds[0] = fake.next_socket_fds[0];
		fds[1] = fake.next_socket_fds[1];
		return FakeResult(fake, "socketpair");
	}

	inline auto FakeDuplicateFd(void *context, int fd, int minimum_fd) -> int {
		auto &fake = *static_cast<AuthHelperSpawnFake *>(context);
		fake.duplicate_fd_requests.emplace_back(fd, minimum_fd);
		fake.operations.emplace_back("duplicate_fd");
		if (fake.fail_operation == "duplicate_fd") {
			errno = EIO;
			return -1;
		}
		return fake.next_duplicate_fd++;
	}

	inline auto FakeActionsInit(void *context, posix_spawn_file_actions_t *actions) -> int {
		(void)actions;
		return FakeResult(*static_cast<AuthHelperSpawnFake *>(context), "actions_init");
	}

	inline auto FakeActionsAdddup2(void *context, posix_spawn_file_actions_t *actions,
	                               int source_fd, int target_fd) -> int {
		(void)actions;
		auto &fake = *static_cast<AuthHelperSpawnFake *>(context);
		fake.dup2_fds.emplace_back(source_fd, target_fd);
		if (target_fd == STDOUT_FILENO) {
			return FakeResult(fake, "dup_stdout");
		}
		return FakeResult(fake, target_fd == STDERR_FILENO ? "dup_stderr" : "dup_lease");
	}

	inline auto FakeActionsAddclose(void *context, posix_spawn_file_actions_t *actions, int fd)
	    -> int {
		(void)actions;
		auto &fake = *static_cast<AuthHelperSpawnFake *>(context);
		fake.action_close_fds.push_back(fd);
		static constexpr std::array kNames = {"close_output_read", "close_output_write",
		                                      "close_lease_parent", "close_lease_child"};
		return FakeResult(fake,
		                  kNames[std::min(fake.action_close_fds.size() - 1, kNames.size() - 1)]);
	}

	inline auto FakeActionsAddclosefrom(void *context, posix_spawn_file_actions_t *actions,
	                                    int from_fd) -> int {
		(void)actions;
		auto &fake = *static_cast<AuthHelperSpawnFake *>(context);
		fake.action_closefrom_fds.push_back(from_fd);
		return FakeResult(fake, "closefrom");
	}

	inline auto FakeActionsDestroy(void *context, posix_spawn_file_actions_t *actions) -> int {
		(void)actions;
		auto &fake = *static_cast<AuthHelperSpawnFake *>(context);
		++fake.destroy_calls;
		const int result = FakeResult(fake, "destroy");
		return fake.destroy_fails ? EIO : result;
	}

	inline auto FakeSpawn(const howdy::pam::auth_helper_process::SpawnRequest &request) -> int {
		auto &fake = *static_cast<AuthHelperSpawnFake *>(request.context);
		++fake.spawn_calls;
		fake.spawn_path = request.path;
		for (const auto *argument = request.argv; argument != nullptr && *argument != nullptr;
		     ++argument) {
			fake.spawn_argv.emplace_back(*argument);
		}
		for (const auto *environment = request.envp;
		     environment != nullptr && *environment != nullptr; ++environment) {
			fake.spawn_env.emplace_back(*environment);
		}
		const int result = FakeResult(fake, "spawn");
		if (result != 0) {
			return result;
		}
		const pid_t pid = fork();
		if (pid < 0) {
			return errno;
		}
		if (pid == 0) {
			_exit(EXIT_SUCCESS);
		}
		*request.child_pid = pid;
		fake.spawned_pid   = pid;
		return 0;
	}

	inline auto FakeClose(void *context, int fd) -> int {
		auto &fake = *static_cast<AuthHelperSpawnFake *>(context);
		fake.parent_close_fds.push_back(fd);
		fake.operations.emplace_back("close");
		return 0;
	}

	inline void FakeAuthHelperSpawnLog(void *context, std::string_view message) {
		static_cast<AuthHelperSpawnFake *>(context)->log_messages.emplace_back(message);
	}

	inline auto SpawnOperations(AuthHelperSpawnFake *fake)
	    -> howdy::pam::auth_helper_process::Operations {
		auto operations                 = howdy::pam::auth_helper_process::ProductionOperations();
		operations.context              = fake;
		operations.pipe2                = FakePipe2;
		operations.socketpair           = FakeSocketpair;
		operations.duplicate_fd         = FakeDuplicateFd;
		operations.actions_init         = FakeActionsInit;
		operations.actions_adddup2      = FakeActionsAdddup2;
		operations.actions_addclose     = FakeActionsAddclose;
		operations.actions_addclosefrom = FakeActionsAddclosefrom;
		operations.actions_destroy      = FakeActionsDestroy;
		operations.spawn                = FakeSpawn;
		operations.close                = FakeClose;
		operations.log_observer         = FakeAuthHelperSpawnLog;
		return operations;
	}

	inline auto ExpectClosedExactlyOnce(const AuthHelperSpawnFake &fake,
	                                    const std::vector<int> &expected_fds, std::string_view name)
	    -> bool {
		bool ok = Expect(fake.parent_close_fds.size() == expected_fds.size(),
		                 std::string(name) + " closes expected parent descriptor count");
		for (const int fd : expected_fds) {
			ok &= Expect(
			    std::count(fake.parent_close_fds.begin(), fake.parent_close_fds.end(), fd) == 1,
			    std::string(name) + " closes parent fd " + std::to_string(fd) + " once");
		}
		return ok;
	}

	inline auto ExpectOperationPrefix(const AuthHelperSpawnFake      &fake,
	                                  const std::vector<std::string> &prefix, std::string_view name)
	    -> bool {
		return Expect(fake.operations.size() >= prefix.size() &&
		                  std::equal(prefix.begin(), prefix.end(), fake.operations.begin()),
		              std::string(name) + " stops after expected operation prefix");
	}

	inline auto ExpectLog(const AuthHelperSpawnFake &fake, std::size_t index,
	                      std::string_view operation, std::string_view name) -> bool {
		bool ok = Expect(fake.log_messages.size() > index, std::string(name) + " logs failure");
		if (fake.log_messages.size() <= index) {
			return false;
		}
		ok &= Expect(fake.log_messages[index].contains(operation),
		             std::string(name) + " log names failed operation");
		ok &= Expect(fake.log_messages[index].contains(std::strerror(EIO)) &&
		                 fake.log_messages[index].contains(std::to_string(EIO)),
		             std::string(name) + " log includes error text and number");
		return ok;
	}

}  // namespace howdy::test::runtime_session
