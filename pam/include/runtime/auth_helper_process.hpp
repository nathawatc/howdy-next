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
		OutputReader read_bounded                                                      = nullptr;
		LogObserver  log_observer                                                      = nullptr;
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

	__attribute__((visibility("hidden"))) auto ProductionOperations() -> Operations;
	__attribute__((visibility("hidden"))) auto OutputLimit() -> std::size_t;
	__attribute__((visibility("hidden"))) auto
	ReceiveLeaseDescriptor(int socket_fd, int *lease_fd,
	                       std::chrono::steady_clock::time_point deadline) -> bool;
	__attribute__((visibility("hidden"))) auto
	ValidateLeaseDescriptor(int lease_fd, const std::filesystem::path &root_dir, uid_t owner_uid)
	    -> bool;
	__attribute__((visibility("hidden"))) auto
	PrepareRuntimeAuthFiles(std::string_view username, PreparedRuntimeFiles *prepared,
	                        const Operations &operations) -> bool;
	__attribute__((visibility("hidden"))) auto
	PrepareRuntimeAuthFiles(std::string_view username, PreparedRuntimeFiles *prepared,
	                        const Operations                     &operations,
	                        std::chrono::steady_clock::time_point deadline) -> bool;
	__attribute__((visibility("hidden"))) auto
	ReadOutput(Process process, std::string *output, const Operations &operations,
	           std::chrono::steady_clock::time_point deadline) -> bool;
	__attribute__((visibility("hidden"))) auto ParseOutput(const std::string &output) -> Output;
	__attribute__((visibility("hidden"))) auto WaitForHelper(pid_t child_pid) -> int;

	__attribute__((visibility("hidden"))) auto
	PrepareRuntimeAuthFiles(std::string_view username, PreparedRuntimeFiles *prepared) -> bool;

}  // namespace howdy::pam::auth_helper_process
