#include "runtime/runtime_session_process_test_support.hpp"
#include "runtime/runtime_session_test_groups.hpp"

namespace {
	using namespace howdy::test::runtime_session;

	auto test_auth_helper_spawn_setup_failures() -> bool {
		enum class FailurePoint : std::uint8_t {
			kInit,
			kFirstClose,
			kStdoutDup,
			kStderrDup,
			kFinalWriteClose,
		};

		struct TestCase {
			std::string_view         name;
			FailurePoint             point;
			std::string_view         operation;
			std::size_t              expected_dup_calls;
			std::size_t              expected_close_actions;
			int                      expected_destroy_calls;
			std::vector<std::string> expected_actions;
		};

		const std::vector<TestCase> test_cases = {
		    {
		        .name                   = "actions init",
		        .point                  = FailurePoint::kInit,
		        .operation              = "posix_spawn_file_actions_init",
		        .expected_dup_calls     = 0,
		        .expected_close_actions = 0,
		        .expected_destroy_calls = 0,
		        .expected_actions       = {"actions_init"},
		    },
		    {
		        .name                   = "first child close",
		        .point                  = FailurePoint::kFirstClose,
		        .operation              = "posix_spawn_file_actions_addclose",
		        .expected_dup_calls     = 0,
		        .expected_close_actions = 1,
		        .expected_destroy_calls = 1,
		        .expected_actions = {"actions_init", "actions_addclose_read", "actions_destroy"},
		    },
		    {
		        .name                   = "stdout duplication",
		        .point                  = FailurePoint::kStdoutDup,
		        .operation              = "posix_spawn_file_actions_adddup2",
		        .expected_dup_calls     = 1,
		        .expected_close_actions = 1,
		        .expected_destroy_calls = 1,
		        .expected_actions       = {"actions_init", "actions_addclose_read",
		                                   "actions_adddup2_stdout", "actions_destroy"},
		    },
		    {
		        .name                   = "stderr duplication",
		        .point                  = FailurePoint::kStderrDup,
		        .operation              = "posix_spawn_file_actions_adddup2",
		        .expected_dup_calls     = 2,
		        .expected_close_actions = 1,
		        .expected_destroy_calls = 1,
		        .expected_actions       = {"actions_init", "actions_addclose_read",
		                                   "actions_adddup2_stdout", "actions_adddup2_stderr",
		                                   "actions_destroy"},
		    },
		    {
		        .name                   = "final child write close",
		        .point                  = FailurePoint::kFinalWriteClose,
		        .operation              = "posix_spawn_file_actions_addclose",
		        .expected_dup_calls     = 2,
		        .expected_close_actions = 2,
		        .expected_destroy_calls = 1,
		        .expected_actions       = {"actions_init", "actions_addclose_read",
		                                   "actions_adddup2_stdout", "actions_adddup2_stderr",
		                                   "actions_addclose_write", "actions_destroy"},
		    },
		};

		bool ok = true;
		for (const auto &test_case : test_cases) {
			AuthHelperSpawnFake fake;
			switch (test_case.point) {
				case FailurePoint::kInit:
					fake.actions_init_result = EIO;
					break;
				case FailurePoint::kFirstClose:
					fake.first_close_result = EIO;
					break;
				case FailurePoint::kStdoutDup:
					fake.stdout_dup_result = EIO;
					break;
				case FailurePoint::kStderrDup:
					fake.stderr_dup_result = EIO;
					break;
				case FailurePoint::kFinalWriteClose:
					fake.final_close_result = EIO;
					break;
			}

			howdy::pam::PreparedRuntimeFiles prepared;
			const bool prepared_ok = howdy::pam::auth_helper_process::prepare_runtime_auth_files(
			    "alice", &prepared, spawn_operations(&fake));
			ok &= expect(!prepared_ok, std::string(test_case.name) + " returns false");
			ok &= expect(fake.spawn_calls == 0,
			             std::string(test_case.name) + " does not spawn helper");
			ok &= expect(fake.dup2_fds.size() == test_case.expected_dup_calls,
			             std::string(test_case.name) + " runs no later duplicate actions");
			ok &= expect(fake.action_close_fds.size() == test_case.expected_close_actions,
			             std::string(test_case.name) + " runs no later close actions");
			ok &= expect(fake.actions_destroy_calls == test_case.expected_destroy_calls,
			             std::string(test_case.name) + " follows action destroy rule");
			ok &= expect(action_operations(fake) == test_case.expected_actions,
			             std::string(test_case.name) + " runs no later spawn actions");
			ok &= expect_parent_pipe_closed_once(fake, test_case.name);
			ok &= expect_primary_spawn_error(fake, test_case.operation, EIO, test_case.name);
		}
		return ok;
	}

	auto test_auth_helper_spawn_setup_failure_destroy_failure() -> bool {
		AuthHelperSpawnFake fake{
		    .stdout_dup_result      = EIO,
		    .actions_destroy_result = EIO,
		};
		howdy::pam::PreparedRuntimeFiles prepared;
		const bool prepared_ok = howdy::pam::auth_helper_process::prepare_runtime_auth_files(
		    "alice", &prepared, spawn_operations(&fake));
		return expect(!prepared_ok, "setup and destroy failure returns false") &&
		       expect(fake.spawn_calls == 0, "setup and destroy failure does not spawn helper") &&
		       expect(fake.spawned_pid == -1, "setup and destroy failure creates no child") &&
		       expect(fake.actions_destroy_calls == 1,
		              "setup and destroy failure destroys actions once") &&
		       expect_parent_pipe_closed_once(fake, "setup and destroy failure") &&
		       expect_primary_spawn_error(fake, "posix_spawn_file_actions_adddup2", EIO,
		                                  "setup and destroy failure") &&
		       expect_destroy_error_after_primary(fake, "setup and destroy failure");
	}

	auto test_auth_helper_spawn_failure_destroy_failure() -> bool {
		AuthHelperSpawnFake fake{
		    .actions_destroy_result = EIO,
		    .spawn_result           = EAGAIN,
		};
		howdy::pam::PreparedRuntimeFiles prepared;
		const bool prepared_ok = howdy::pam::auth_helper_process::prepare_runtime_auth_files(
		    "alice", &prepared, spawn_operations(&fake));
		return expect(!prepared_ok, "spawn and destroy failure returns false") &&
		       expect(fake.spawn_calls == 1, "spawn and destroy failure calls spawn once") &&
		       expect(fake.output_reader_calls == 0,
		              "spawn and destroy failure does not read helper output") &&
		       expect(fake.spawned_pid == -1, "spawn and destroy failure creates no child") &&
		       expect(fake.actions_destroy_calls == 1,
		              "spawn and destroy failure destroys actions once") &&
		       expect_parent_pipe_closed_once(fake, "spawn and destroy failure") &&
		       expect_primary_spawn_error(fake, "posix_spawn", EAGAIN,
		                                  "spawn and destroy failure") &&
		       expect_destroy_error_after_primary(fake, "spawn and destroy failure");
	}

	auto test_auth_helper_spawn_destroy_failure() -> bool {
		AuthHelperSpawnFake              fake{.actions_destroy_result = EIO};
		howdy::pam::PreparedRuntimeFiles prepared;
		const bool prepared_ok = howdy::pam::auth_helper_process::prepare_runtime_auth_files(
		    "alice", &prepared, spawn_operations(&fake));
		return expect(prepared_ok, "post-spawn destroy failure preserves helper success") &&
		       expect(fake.spawn_calls == 1, "destroy failure occurs after helper spawn") &&
		       expect(fake.output_reader_calls == 1, "destroy failure still reads helper output") &&
		       expect(fake.actions_destroy_calls == 1, "destroy failure destroys actions once") &&
		       expect_parent_pipe_closed_once(fake, "destroy failure") &&
		       expect(prepared.config_path == "/run/howdy/auth-helper/config.ini",
		              "destroy failure preserves prepared config path") &&
		       expect(prepared.user_models_dir == "/run/howdy/auth-helper/models",
		              "destroy failure preserves prepared models path") &&
		       expect(prepared.root_dir == "/run/howdy/auth-helper",
		              "destroy failure preserves prepared root path") &&
		       expect_primary_spawn_error(fake, "posix_spawn_file_actions_destroy", EIO,
		                                  "destroy failure") &&
		       expect_child_reaped(&fake, "destroy failure");
	}

	auto test_auth_helper_spawn_failure() -> bool {
		AuthHelperSpawnFake              fake{.spawn_result = EAGAIN};
		howdy::pam::PreparedRuntimeFiles prepared;
		const bool prepared_ok = howdy::pam::auth_helper_process::prepare_runtime_auth_files(
		    "alice", &prepared, spawn_operations(&fake));
		return expect(!prepared_ok, "spawn failure returns false") &&
		       expect(fake.spawn_calls == 1, "spawn failure calls spawn once") &&
		       expect(fake.output_reader_calls == 0, "spawn failure does not read helper output") &&
		       expect(fake.actions_destroy_calls == 1, "spawn failure destroys actions once") &&
		       expect_parent_pipe_closed_once(fake, "spawn failure") &&
		       expect_primary_spawn_error(fake, "posix_spawn", EAGAIN, "spawn failure");
	}

	auto test_auth_helper_spawn_success() -> bool {
		AuthHelperSpawnFake              fake;
		howdy::pam::PreparedRuntimeFiles prepared;
		const bool prepared_ok = howdy::pam::auth_helper_process::prepare_runtime_auth_files(
		    "alice", &prepared, spawn_operations(&fake));
		return expect(prepared_ok, "spawn success returns true") &&
		       expect(fake.pipe_fds == std::vector<std::array<int, 2>>{{10, 11}},
		              "spawn success uses expected pipe fds") &&
		       expect(fake.pipe_flags == std::vector<int>{O_CLOEXEC},
		              "spawn success creates close-on-exec pipe") &&
		       expect(fake.spawn_path == kAuthHelperPath, "spawn success uses auth helper path") &&
		       expect(fake.spawn_argv ==
		                  std::vector<std::string>{kAuthHelperPath, "prepare", "alice"},
		              "spawn success uses expected argv") &&
		       expect(fake.spawn_env.empty(), "spawn success uses empty environment") &&
		       expect(fake.dup2_fds == std::vector<std::pair<int, int>>{{11, STDOUT_FILENO},
		                                                                {11, STDERR_FILENO}},
		              "spawn success redirects stdout and stderr") &&
		       expect(fake.action_close_fds == std::vector<int>{10, 11},
		              "spawn success closes both pipe fds in child") &&
		       expect(action_operations(fake) ==
		                  std::vector<std::string>{"actions_init", "actions_addclose_read",
		                                           "actions_adddup2_stdout",
		                                           "actions_adddup2_stderr",
		                                           "actions_addclose_write", "actions_destroy"},
		              "spawn success performs child actions in order") &&
		       expect(fake.actions_destroy_calls == 1, "spawn success destroys actions once") &&
		       expect(fake.output_reader_calls == 1, "spawn success reads helper output once") &&
		       expect(fake.parent_close_fds == std::vector<int>{11, 10},
		              "spawn success closes parent write then read fd") &&
		       expect(
		           fake.operations ==
		               std::vector<std::string>{"pipe2", "actions_init", "actions_addclose_read",
		                                        "actions_adddup2_stdout", "actions_adddup2_stderr",
		                                        "actions_addclose_write", "spawn",
		                                        "actions_destroy", "close", "read_output", "close"},
		           "spawn success performs helper operations in order") &&
		       expect_parent_pipe_closed_once(fake, "spawn success") &&
		       expect(prepared.config_path == "/run/howdy/auth-helper/config.ini",
		              "spawn success reads config path") &&
		       expect(prepared.user_models_dir == "/run/howdy/auth-helper/models",
		              "spawn success reads models path") &&
		       expect(prepared.root_dir == "/run/howdy/auth-helper",
		              "spawn success derives runtime root") &&
		       expect_child_reaped(&fake, "spawn success");
	}

	auto integration_pipe2(void *context, int *pipe_fds, int flags) -> int {
		(void)context;
		const int result = pipe2(pipe_fds, flags);
		if (result != 0 || (pipe_fds[0] == STDOUT_FILENO && pipe_fds[1] == STDERR_FILENO)) {
			return result;
		}
		(void)close(pipe_fds[0]);
		(void)close(pipe_fds[1]);
		errno = EBUSY;
		return -1;
	}

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

	auto integration_spawn(const howdy::pam::auth_helper_process::SpawnRequest &request) -> int {
		std::array<char *, 4> shell_args = {
		    const_cast<char *>("/bin/sh"),
		    const_cast<char *>("-c"),
		    const_cast<char *>("printf 'CONFIG_PATH=/run/howdy/collision/config.ini\\n'; "
		                       "printf 'USER_MODELS_DIR=/run/howdy/collision/models\\n' >&2"),
		    nullptr,
		};
		std::array<char *, 1> empty_env = {nullptr};
		return posix_spawn(request.child_pid, "/bin/sh", request.actions, nullptr,
		                   shell_args.data(), empty_env.data());
	}

	auto integration_close(void *context, int fd) -> int {
		(void)context;
		return close(fd);
	}

	auto test_auth_helper_spawn_real_descriptor_collisions() -> bool {
		const pid_t test_pid = fork();
		if (test_pid == 0) {
			(void)close(STDOUT_FILENO);
			(void)close(STDERR_FILENO);
			howdy::pam::PreparedRuntimeFiles prepared;
			auto operations             = howdy::pam::auth_helper_process::production_operations();
			operations.pipe2            = integration_pipe2;
			operations.duplicate_fd     = integration_duplicate_fd;
			operations.actions_init     = integration_actions_init;
			operations.actions_adddup2  = integration_actions_adddup2;
			operations.actions_addclose = integration_actions_addclose;
			operations.actions_destroy  = integration_actions_destroy;
			operations.spawn            = integration_spawn;
			operations.close            = integration_close;
			operations.read_bounded     = nullptr;
			const bool prepared_ok = howdy::pam::auth_helper_process::prepare_runtime_auth_files(
			    "alice", &prepared, operations);
			const bool paths_ok = prepared.config_path == "/run/howdy/collision/config.ini" &&
			                      prepared.user_models_dir == "/run/howdy/collision/models" &&
			                      prepared.root_dir == "/run/howdy/collision";
			_exit(prepared_ok && paths_ok ? EXIT_SUCCESS : EXIT_FAILURE);
		}
		if (test_pid < 0) {
			return expect(false, "real descriptor collision test forks subprocess");
		}

		int status = 0;
		while (waitpid(test_pid, &status, 0) < 0) {
			if (errno != EINTR) {
				return expect(false, "real descriptor collision test waits for subprocess");
			}
		}
		return expect(WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS,
		              "real file actions preserve stdout and stderr across exec");
	}

	auto test_auth_helper_spawn_descriptor_collisions() -> bool {
		const std::vector<std::array<int, 2>> pipe_fd_pairs = {
		    {STDOUT_FILENO, STDERR_FILENO},
		    {STDERR_FILENO, STDOUT_FILENO},
		};

		bool ok = true;
		for (const auto &pipe_fds : pipe_fd_pairs) {
			AuthHelperSpawnFake              fake{.next_pipe_fds = pipe_fds};
			howdy::pam::PreparedRuntimeFiles prepared;
			const bool prepared_ok = howdy::pam::auth_helper_process::prepare_runtime_auth_files(
			    "alice", &prepared, spawn_operations(&fake));
			const char *const name =
			    pipe_fds[0] == STDOUT_FILENO ? "stdout-read collision" : "stderr-read collision";
			ok &= expect(prepared_ok, std::string(name) + " succeeds");
			ok &= expect(fake.duplicate_fd_requests ==
			                 std::vector<std::pair<int, int>>{{pipe_fds[0], STDERR_FILENO + 1},
			                                                  {pipe_fds[1], STDERR_FILENO + 1}},
			             std::string(name) + " normalizes both pipe descriptors");
			ok &= expect(fake.action_close_fds == std::vector<int>{20, 21},
			             std::string(name) + " closes normalized pipe fds in child");
			ok &= expect(action_operations(fake) ==
			                 std::vector<std::string>{"actions_init", "actions_addclose_read",
			                                          "actions_adddup2_stdout",
			                                          "actions_adddup2_stderr",
			                                          "actions_addclose_write", "actions_destroy"},
			             std::string(name) + " uses collision-free child actions");
			ok &= expect(fake.dup2_fds == std::vector<std::pair<int, int>>{{21, STDOUT_FILENO},
			                                                               {21, STDERR_FILENO}},
			             std::string(name) + " redirects normalized write fd");
			ok &=
			    expect(fake.parent_close_fds == std::vector<int>{pipe_fds[0], pipe_fds[1], 21, 20},
			           std::string(name) + " closes original and normalized fds once");
			ok &= expect_parent_pipe_closed_once(fake, name);
			ok &= expect_child_reaped(&fake, name);
		}
		return ok;
	}

}  // namespace

auto run_runtime_session_spawn_tests() -> bool {
	bool ok = true;
	ok &= test_auth_helper_spawn_setup_failures();
	ok &= test_auth_helper_spawn_setup_failure_destroy_failure();
	ok &= test_auth_helper_spawn_failure_destroy_failure();
	ok &= test_auth_helper_spawn_destroy_failure();
	ok &= test_auth_helper_spawn_failure();
	ok &= test_auth_helper_spawn_success();
	ok &= test_auth_helper_spawn_descriptor_collisions();
	ok &= test_auth_helper_spawn_real_descriptor_collisions();
	return ok;
}
