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
	using howdy::test::expect;

	inline auto test_prepared_runtime_root(std::string_view suffix = "helper")
	    -> std::filesystem::path {
		(void)suffix;
		return howdy::native::auth_helper_protocol::prepared_runtime_generation_dir(
		    howdy::native::auth_helper_protocol::prepared_runtime_root(), getuid(),
		    howdy::native::auth_helper_protocol::RuntimeGenerationSlot::kSlot0);
	}

	inline auto test_prepared_runtime_config_path(std::string_view suffix = "helper")
	    -> std::filesystem::path {
		return howdy::native::auth_helper_protocol::prepared_config_path(
		    test_prepared_runtime_root(suffix));
	}

	inline auto test_prepared_runtime_models_dir(std::string_view suffix = "helper")
	    -> std::filesystem::path {
		return howdy::native::auth_helper_protocol::prepared_user_models_dir(
		    test_prepared_runtime_root(suffix));
	}

	inline auto test_prepared_runtime_output(std::string_view suffix = "helper") -> std::string {
		return "CONFIG_PATH=" + test_prepared_runtime_config_path(suffix).string() +
		       "\nUSER_MODELS_DIR=" + test_prepared_runtime_models_dir(suffix).string() + "\n";
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

	inline auto fake_result(AuthHelperSpawnFake &fake, std::string_view operation) -> int {
		fake.operations.emplace_back(operation);
		if (fake.fail_operation != operation) {
			return 0;
		}
		errno = EIO;
		return EIO;
	}

	inline auto fake_pipe2(void *context, int *fds, int flags) -> int {
		auto &fake = *static_cast<AuthHelperSpawnFake *>(context);
		fake.pipe_flags.push_back(flags);
		fds[0] = fake.next_pipe_fds[0];
		fds[1] = fake.next_pipe_fds[1];
		return fake_result(fake, "pipe2");
	}

	inline auto fake_socketpair(void *context, int domain, int type, int protocol, int *fds)
	    -> int {
		auto &fake = *static_cast<AuthHelperSpawnFake *>(context);
		fake.socket_parameters.push_back({domain, type, protocol});
		fds[0] = fake.next_socket_fds[0];
		fds[1] = fake.next_socket_fds[1];
		return fake_result(fake, "socketpair");
	}

	inline auto fake_duplicate_fd(void *context, int fd, int minimum_fd) -> int {
		auto &fake = *static_cast<AuthHelperSpawnFake *>(context);
		fake.duplicate_fd_requests.emplace_back(fd, minimum_fd);
		fake.operations.emplace_back("duplicate_fd");
		if (fake.fail_operation == "duplicate_fd") {
			errno = EIO;
			return -1;
		}
		return fake.next_duplicate_fd++;
	}

	inline auto fake_actions_init(void *context, posix_spawn_file_actions_t *actions) -> int {
		(void)actions;
		return fake_result(*static_cast<AuthHelperSpawnFake *>(context), "actions_init");
	}

	inline auto fake_actions_adddup2(void *context, posix_spawn_file_actions_t *actions,
	                                 int source_fd, int target_fd) -> int {
		(void)actions;
		auto &fake = *static_cast<AuthHelperSpawnFake *>(context);
		fake.dup2_fds.emplace_back(source_fd, target_fd);
		if (target_fd == STDOUT_FILENO) {
			return fake_result(fake, "dup_stdout");
		}
		return fake_result(fake, target_fd == STDERR_FILENO ? "dup_stderr" : "dup_lease");
	}

	inline auto fake_actions_addclose(void *context, posix_spawn_file_actions_t *actions, int fd)
	    -> int {
		(void)actions;
		auto &fake = *static_cast<AuthHelperSpawnFake *>(context);
		fake.action_close_fds.push_back(fd);
		static constexpr std::array names = {"close_output_read", "close_output_write",
		                                     "close_lease_parent", "close_lease_child"};
		return fake_result(fake,
		                   names[std::min(fake.action_close_fds.size() - 1, names.size() - 1)]);
	}

	inline auto fake_actions_addclosefrom(void *context, posix_spawn_file_actions_t *actions,
	                                      int from_fd) -> int {
		(void)actions;
		auto &fake = *static_cast<AuthHelperSpawnFake *>(context);
		fake.action_closefrom_fds.push_back(from_fd);
		return fake_result(fake, "closefrom");
	}

	inline auto fake_actions_destroy(void *context, posix_spawn_file_actions_t *actions) -> int {
		(void)actions;
		auto &fake = *static_cast<AuthHelperSpawnFake *>(context);
		++fake.destroy_calls;
		const int result = fake_result(fake, "destroy");
		return fake.destroy_fails ? EIO : result;
	}

	inline auto fake_spawn(const howdy::pam::auth_helper_process::SpawnRequest &request) -> int {
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
		const int result = fake_result(fake, "spawn");
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

	inline auto fake_close(void *context, int fd) -> int {
		auto &fake = *static_cast<AuthHelperSpawnFake *>(context);
		fake.parent_close_fds.push_back(fd);
		fake.operations.emplace_back("close");
		return 0;
	}

	inline void fake_auth_helper_spawn_log(void *context, std::string_view message) {
		static_cast<AuthHelperSpawnFake *>(context)->log_messages.emplace_back(message);
	}

	inline auto spawn_operations(AuthHelperSpawnFake *fake)
	    -> howdy::pam::auth_helper_process::Operations {
		auto operations                 = howdy::pam::auth_helper_process::production_operations();
		operations.context              = fake;
		operations.pipe2                = fake_pipe2;
		operations.socketpair           = fake_socketpair;
		operations.duplicate_fd         = fake_duplicate_fd;
		operations.actions_init         = fake_actions_init;
		operations.actions_adddup2      = fake_actions_adddup2;
		operations.actions_addclose     = fake_actions_addclose;
		operations.actions_addclosefrom = fake_actions_addclosefrom;
		operations.actions_destroy      = fake_actions_destroy;
		operations.spawn                = fake_spawn;
		operations.close                = fake_close;
		operations.log_observer         = fake_auth_helper_spawn_log;
		return operations;
	}

	inline auto expect_closed_exactly_once(const AuthHelperSpawnFake &fake,
	                                       const std::vector<int>    &expected_fds,
	                                       std::string_view           name) -> bool {
		bool ok = expect(fake.parent_close_fds.size() == expected_fds.size(),
		                 std::string(name) + " closes expected parent descriptor count");
		for (const int fd : expected_fds) {
			ok &= expect(
			    std::count(fake.parent_close_fds.begin(), fake.parent_close_fds.end(), fd) == 1,
			    std::string(name) + " closes parent fd " + std::to_string(fd) + " once");
		}
		return ok;
	}

	inline auto expect_operation_prefix(const AuthHelperSpawnFake      &fake,
	                                    const std::vector<std::string> &prefix,
	                                    std::string_view                name) -> bool {
		return expect(fake.operations.size() >= prefix.size() &&
		                  std::equal(prefix.begin(), prefix.end(), fake.operations.begin()),
		              std::string(name) + " stops after expected operation prefix");
	}

	inline auto expect_log(const AuthHelperSpawnFake &fake, std::size_t index,
	                       std::string_view operation, std::string_view name) -> bool {
		bool ok = expect(fake.log_messages.size() > index, std::string(name) + " logs failure");
		if (fake.log_messages.size() <= index) {
			return false;
		}
		ok &= expect(fake.log_messages[index].contains(operation),
		             std::string(name) + " log names failed operation");
		ok &= expect(fake.log_messages[index].contains(std::strerror(EIO)) &&
		                 fake.log_messages[index].contains(std::to_string(EIO)),
		             std::string(name) + " log includes error text and number");
		return ok;
	}

}  // namespace howdy::test::runtime_session
