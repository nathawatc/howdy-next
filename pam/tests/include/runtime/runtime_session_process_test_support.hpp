#pragma once
#include "runtime/auth_helper_process.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <paths.hpp>
#include <spawn.h>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

#include <sys/wait.h>

namespace howdy::test::runtime_session {
	using howdy::test::expect;

	struct AuthHelperSpawnFake {
		std::vector<std::string>         operations;
		std::vector<std::array<int, 2>>  pipe_fds;
		std::vector<int>                 pipe_flags;
		std::vector<std::pair<int, int>> duplicate_fd_requests;
		std::vector<std::pair<int, int>> dup2_fds;
		std::vector<int>                 action_close_fds;
		std::vector<int>                 action_closefrom_fds;
		std::vector<int>                 parent_close_fds;
		std::vector<std::string>         spawn_argv;
		std::vector<std::string>         spawn_env;
		std::vector<std::string>         log_messages;
		std::string                      spawn_path;
		std::array<int, 2>               next_pipe_fds          = {10, 11};
		int                              next_duplicate_fd      = 20;
		int                              actions_init_result    = 0;
		int                              stdout_dup_result      = 0;
		int                              stderr_dup_result      = 0;
		int                              first_close_result     = 0;
		int                              final_close_result     = 0;
		int                              closefrom_result       = 0;
		int                              actions_destroy_result = 0;
		int                              spawn_result           = 0;
		int                              actions_init_calls     = 0;
		int                              actions_destroy_calls  = 0;
		int                              spawn_calls            = 0;
		int                              output_reader_calls    = 0;
		pid_t                            spawned_pid            = -1;
	};

	inline auto fake_pipe2(void *context, int *pipe_fds, int flags) -> int {
		auto &fake = *static_cast<AuthHelperSpawnFake *>(context);
		fake.operations.emplace_back("pipe2");
		fake.pipe_fds.push_back(fake.next_pipe_fds);
		fake.pipe_flags.push_back(flags);
		pipe_fds[0] = fake.next_pipe_fds[0];
		pipe_fds[1] = fake.next_pipe_fds[1];
		return 0;
	}

	inline auto fake_duplicate_fd(void *context, int fd, int minimum_fd) -> int {
		auto &fake = *static_cast<AuthHelperSpawnFake *>(context);
		fake.operations.emplace_back("duplicate_fd");
		fake.duplicate_fd_requests.emplace_back(fd, minimum_fd);
		return fake.next_duplicate_fd++;
	}

	inline auto fake_actions_init(void *context, posix_spawn_file_actions_t *actions) -> int {
		(void)actions;
		auto &fake = *static_cast<AuthHelperSpawnFake *>(context);
		fake.operations.emplace_back("actions_init");
		++fake.actions_init_calls;
		return fake.actions_init_result;
	}

	inline auto fake_actions_adddup2(void *context, posix_spawn_file_actions_t *actions, int old_fd,
	                                 int new_fd) -> int {
		(void)actions;
		auto &fake = *static_cast<AuthHelperSpawnFake *>(context);
		fake.operations.emplace_back(new_fd == STDOUT_FILENO ? "actions_adddup2_stdout"
		                                                     : "actions_adddup2_stderr");
		fake.dup2_fds.emplace_back(old_fd, new_fd);
		return new_fd == STDOUT_FILENO ? fake.stdout_dup_result : fake.stderr_dup_result;
	}

	inline auto fake_actions_addclose(void *context, posix_spawn_file_actions_t *actions, int fd)
	    -> int {
		(void)actions;
		auto &fake = *static_cast<AuthHelperSpawnFake *>(context);
		fake.operations.emplace_back(fake.action_close_fds.empty() ? "actions_addclose_read"
		                                                           : "actions_addclose_write");
		fake.action_close_fds.push_back(fd);
		return fake.action_close_fds.size() == 1 ? fake.first_close_result
		                                         : fake.final_close_result;
	}

	inline auto fake_actions_addclosefrom(void *context, posix_spawn_file_actions_t *actions,
	                                      int from_fd) -> int {
		(void)actions;
		auto &fake = *static_cast<AuthHelperSpawnFake *>(context);
		fake.operations.emplace_back("actions_addclosefrom");
		fake.action_closefrom_fds.push_back(from_fd);
		return fake.closefrom_result;
	}

	inline auto fake_actions_destroy(void *context, posix_spawn_file_actions_t *actions) -> int {
		(void)actions;
		auto &fake = *static_cast<AuthHelperSpawnFake *>(context);
		fake.operations.emplace_back("actions_destroy");
		++fake.actions_destroy_calls;
		return fake.actions_destroy_result;
	}

	inline auto fake_spawn(const howdy::pam::auth_helper_process::SpawnRequest &request) -> int {
		auto &fake = *static_cast<AuthHelperSpawnFake *>(request.context);
		fake.operations.emplace_back("spawn");
		++fake.spawn_calls;
		fake.spawn_path = request.path;
		for (auto *const *argument = request.argv; argument != nullptr && *argument != nullptr;
		     ++argument) {
			fake.spawn_argv.emplace_back(*argument);
		}
		for (auto *const *environment = request.envp;
		     environment != nullptr && *environment != nullptr; ++environment) {
			fake.spawn_env.emplace_back(*environment);
		}
		if (fake.spawn_result != 0) {
			return fake.spawn_result;
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
		fake.operations.emplace_back("close");
		fake.parent_close_fds.push_back(fd);
		return 0;
	}

	inline auto
	fake_auth_helper_output_reader(void                                              *context,
	                               [[maybe_unused]] howdy::native::BoundedReadRequest request)
	    -> howdy::native::BoundedReadResult {
		auto &fake = *static_cast<AuthHelperSpawnFake *>(context);
		fake.operations.emplace_back("read_output");
		++fake.output_reader_calls;
		return {
		    .output = "CONFIG_PATH=/run/howdy/auth-helper/config.ini\n"
		              "USER_MODELS_DIR=/run/howdy/auth-helper/models\n",
		};
	}

	inline auto fake_auth_helper_spawn_log(void *context, std::string_view message) -> void {
		static_cast<AuthHelperSpawnFake *>(context)->log_messages.emplace_back(message);
	}

	inline auto spawn_operations(AuthHelperSpawnFake *fake)
	    -> howdy::pam::auth_helper_process::Operations {
		auto operations                 = howdy::pam::auth_helper_process::production_operations();
		operations.context              = fake;
		operations.pipe2                = fake_pipe2;
		operations.duplicate_fd         = fake_duplicate_fd;
		operations.actions_init         = fake_actions_init;
		operations.actions_adddup2      = fake_actions_adddup2;
		operations.actions_addclose     = fake_actions_addclose;
		operations.actions_addclosefrom = fake_actions_addclosefrom;
		operations.actions_destroy      = fake_actions_destroy;
		operations.spawn                = fake_spawn;
		operations.close                = fake_close;
		operations.read_bounded         = fake_auth_helper_output_reader;
		operations.log_observer         = fake_auth_helper_spawn_log;
		return operations;
	}

	inline auto expect_parent_pipe_closed_once(const AuthHelperSpawnFake &fake,
	                                           std::string_view           name) -> bool {
		return expect(std::count(fake.parent_close_fds.begin(), fake.parent_close_fds.end(),
		                         fake.next_pipe_fds[0]) == 1,
		              std::string(name) + " closes parent read fd once") &&
		       expect(std::count(fake.parent_close_fds.begin(), fake.parent_close_fds.end(),
		                         fake.next_pipe_fds[1]) == 1,
		              std::string(name) + " closes parent write fd once");
	}

	inline auto action_operations(const AuthHelperSpawnFake &fake) -> std::vector<std::string> {
		std::vector<std::string> actions;
		for (const auto &operation : fake.operations) {
			if (operation.starts_with("actions_")) {
				actions.push_back(operation);
			}
		}
		return actions;
	}

	inline auto expect_primary_spawn_error(const AuthHelperSpawnFake &fake,
	                                       std::string_view operation, int error_code,
	                                       std::string_view name) -> bool {
		return expect(!fake.log_messages.empty(), std::string(name) + " logs primary error") &&
		       expect(fake.log_messages.front().contains(operation),
		              std::string(name) + " first log names failed operation") &&
		       expect(fake.log_messages.front().contains(std::strerror(error_code)),
		              std::string(name) + " first log includes strerror") &&
		       expect(fake.log_messages.front().contains(std::to_string(error_code)),
		              std::string(name) + " first log includes numeric error");
	}

	inline auto expect_destroy_error_after_primary(const AuthHelperSpawnFake &fake,
	                                               std::string_view           name) -> bool {
		return expect(fake.log_messages.size() >= 2,
		              std::string(name) + " logs destroy error after primary error") &&
		       expect(fake.log_messages[1].contains("posix_spawn_file_actions_destroy"),
		              std::string(name) + " second log names action destroy") &&
		       expect(fake.log_messages[1].contains(std::strerror(EIO)),
		              std::string(name) + " second log includes destroy strerror") &&
		       expect(fake.log_messages[1].contains(std::to_string(EIO)),
		              std::string(name) + " second log includes destroy numeric error");
	}

	inline auto expect_child_reaped(AuthHelperSpawnFake *fake, std::string_view name) -> bool {
		if (fake->spawned_pid < 0) {
			return true;
		}

		int status             = 0;
		errno                  = 0;
		const auto wait_result = waitpid(fake->spawned_pid, &status, WNOHANG);
		if (wait_result == fake->spawned_pid) {
			return expect(false, std::string(name) + " leaves no unreaped helper child");
		}
		if (wait_result == 0) {
			(void)waitpid(fake->spawned_pid, &status, 0);
			return expect(false, std::string(name) + " leaves no active helper child");
		}
		return expect(wait_result == -1 && errno == ECHILD,
		              std::string(name) + " reaps helper child");
	}

}  // namespace howdy::test::runtime_session
