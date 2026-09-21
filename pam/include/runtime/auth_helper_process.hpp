#pragma once

#include "runtime/runtime_session.hpp"

#include <chrono>
#include <span>
#include <string>
#include <string_view>

#include <spawn.h>

namespace howdy::pam::auth_helper_process {

	struct SpawnRequest {
		void                             *context   = nullptr;
		pid_t                            *child_pid = nullptr;
		const char                       *path      = nullptr;
		const posix_spawn_file_actions_t *actions   = nullptr;
		std::span<char *const>            argv;
		std::span<char *const>            envp;
	};

	using LogObserver = void (*)(void *context, std::string_view message);

	struct Operations {
		void *context = nullptr;

		int (*pipe2)(void *, int *pipe_fds, int flags)                                 = nullptr;
		int (*socketpair)(void *, int domain, int type, int protocol, int *socket_fds) = nullptr;
		int (*duplicate_fd)(void *, int fd, int minimum_fd)                            = nullptr;
		int (*actions_init)(void *, posix_spawn_file_actions_t *)                      = nullptr;
		int (*actions_adddup2)(void *, posix_spawn_file_actions_t *, int source_fd,
		                       int target_fd)                                          = nullptr;
		int (*actions_addclose)(void *, posix_spawn_file_actions_t *, int fd)          = nullptr;
		int (*actions_addclosefrom)(void *, posix_spawn_file_actions_t *, int from_fd) = nullptr;
		int (*actions_destroy)(void *, posix_spawn_file_actions_t *)                   = nullptr;
		int (*spawn)(const SpawnRequest &)                                             = nullptr;
		int (*close)(void *, int fd)                                                   = nullptr;
		LogObserver log_observer                                                       = nullptr;
	};

	struct Output {
		std::string config_path;
		std::string user_models_dir;
		bool        valid = false;
	};

	__attribute__((visibility("hidden"))) auto ProductionOperations() -> Operations;
	__attribute__((visibility("hidden"))) auto
	PrepareRuntimeAuthFiles(std::string_view username, PreparedRuntimeFiles *prepared,
	                        const Operations &operations) -> bool;
	__attribute__((visibility("hidden"))) auto
	PrepareRuntimeAuthFiles(std::string_view username, PreparedRuntimeFiles *prepared,
	                        const Operations                     &operations,
	                        std::chrono::steady_clock::time_point deadline) -> bool;

	__attribute__((visibility("hidden"))) auto
	PrepareRuntimeAuthFiles(std::string_view username, PreparedRuntimeFiles *prepared) -> bool;

}  // namespace howdy::pam::auth_helper_process
