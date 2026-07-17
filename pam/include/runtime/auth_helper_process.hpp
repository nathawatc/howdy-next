#pragma once

#include "runtime/runtime_session.hpp"
#include "support/fd_io.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <spawn.h>
#include <string>
#include <string_view>

namespace howdy::pam::auth_helper_process {

	struct SpawnRequest {
		void                             *context   = nullptr;
		pid_t                            *child_pid = nullptr;
		const char                       *path      = nullptr;
		const posix_spawn_file_actions_t *actions   = nullptr;
		char *const                      *argv      = nullptr;
		char *const                      *envp      = nullptr;
	};

	using OutputReader = howdy::native::BoundedReadResult (*)(
	    void *context, howdy::native::BoundedReadRequest request);
	using LogObserver = void (*)(void *context, std::string_view message);

	struct Operations {
		void *context = nullptr;

		int (*pipe2)(void *, int *pipe_fds, int flags)                        = nullptr;
		int (*duplicate_fd)(void *, int fd, int minimum_fd)                   = nullptr;
		int (*actions_init)(void *, posix_spawn_file_actions_t *)             = nullptr;
		int (*actions_adddup2)(void *, posix_spawn_file_actions_t *, int source_fd,
		                       int target_fd)                                 = nullptr;
		int (*actions_addclose)(void *, posix_spawn_file_actions_t *, int fd) = nullptr;
		int (*actions_destroy)(void *, posix_spawn_file_actions_t *)          = nullptr;
		int (*spawn)(const SpawnRequest &)                                    = nullptr;
		int (*close)(void *, int fd)                                          = nullptr;
		OutputReader read_bounded                                             = nullptr;
		LogObserver  log_observer                                             = nullptr;
	};

	struct Output {
		std::string config_path;
		std::string user_models_dir;
		bool        valid = false;
	};

	struct Process {
		pid_t child_pid = -1;
		int   output_fd = -1;
	};

	__attribute__((visibility("hidden"))) auto production_operations() -> Operations;
	__attribute__((visibility("hidden"))) auto output_limit() -> std::size_t;
	__attribute__((visibility("hidden"))) auto
	prepare_runtime_auth_files(std::string_view username, PreparedRuntimeFiles *prepared,
	                           const Operations &operations) -> bool;
	__attribute__((visibility("hidden"))) auto
	prepare_runtime_auth_files(std::string_view username, PreparedRuntimeFiles *prepared,
	                           const Operations                     &operations,
	                           std::chrono::steady_clock::time_point deadline) -> bool;
	__attribute__((visibility("hidden"))) auto
	cleanup_runtime_auth_files(const std::filesystem::path &root_dir, const Operations &operations,
	                           std::chrono::steady_clock::time_point deadline) -> void;
	__attribute__((visibility("hidden"))) auto
	read_output(Process process, std::string *output, const Operations &operations,
	            std::chrono::steady_clock::time_point deadline) -> bool;
	__attribute__((visibility("hidden"))) auto
	wait_for_cleanup_helper(pid_t child_pid, const Operations &operations,
	                        std::chrono::steady_clock::time_point deadline) -> bool;
	__attribute__((visibility("hidden"))) auto parse_output(const std::string &output) -> Output;
	__attribute__((visibility("hidden"))) auto wait_for_helper(pid_t child_pid) -> int;

	__attribute__((visibility("hidden"))) auto
	prepare_runtime_auth_files(std::string_view username, PreparedRuntimeFiles *prepared) -> bool;
	__attribute__((visibility("hidden"))) auto
	cleanup_runtime_auth_files(const std::filesystem::path &root_dir) -> void;

}  // namespace howdy::pam::auth_helper_process
