#pragma once

#ifdef HOWDY_PAM_TESTING

#	include "runtime_session.hpp"

#	include <chrono>
#	include <string>
#	include <string_view>

#	include <spawn.h>

namespace howdy::pam::testing {
	struct AuthHelperSpawnRequest {
		void                             *context   = nullptr;
		pid_t                            *child_pid = nullptr;
		const char                       *path      = nullptr;
		const posix_spawn_file_actions_t *actions   = nullptr;
		char *const                      *argv      = nullptr;
		char *const                      *envp      = nullptr;
	};

	struct AuthHelperSpawnOperations {
		void *context = nullptr;

		int (*pipe2_fn)(void *, int *pipe_fds, int flags)                        = nullptr;
		int (*duplicate_fd_fn)(void *, int fd, int minimum_fd)                   = nullptr;
		int (*actions_init_fn)(void *, posix_spawn_file_actions_t *)             = nullptr;
		int (*actions_adddup2_fn)(void *, posix_spawn_file_actions_t *, int source_fd,
		                          int target_fd)                                 = nullptr;
		int (*actions_addclose_fn)(void *, posix_spawn_file_actions_t *, int fd) = nullptr;
		int (*actions_destroy_fn)(void *, posix_spawn_file_actions_t *)          = nullptr;
		int (*spawn_fn)(const AuthHelperSpawnRequest &)                          = nullptr;
		int (*close_fn)(void *, int fd)                                          = nullptr;
	};

	using AuthHelperSpawnLogFn = void (*)(std::string_view message);

	struct AuthHelperProcess {
		pid_t child_pid = -1;
		int   output_fd = -1;
	};

	auto prepare_runtime_auth_files(std::string_view username, PreparedRuntimeFiles *prepared,
	                                const AuthHelperSpawnOperations &operations) -> bool;
	auto prepare_runtime_auth_files_until(std::string_view username, PreparedRuntimeFiles *prepared,
	                                      const AuthHelperSpawnOperations      &operations,
	                                      std::chrono::steady_clock::time_point deadline) -> bool;
	auto cleanup_runtime_auth_files_until(const std::filesystem::path          &root_dir,
	                                      const AuthHelperSpawnOperations      &operations,
	                                      std::chrono::steady_clock::time_point deadline) -> void;
	auto set_auth_helper_spawn_log_fn(AuthHelperSpawnLogFn logger) -> AuthHelperSpawnLogFn;
	auto read_auth_helper_output_until(AuthHelperProcess process, std::string *output,
	                                   std::chrono::steady_clock::time_point deadline) -> bool;
	auto wait_for_cleanup_helper_until(pid_t                                 child_pid,
	                                   std::chrono::steady_clock::time_point deadline) -> bool;

}  // namespace howdy::pam::testing

#endif
