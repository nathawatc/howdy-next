#pragma once

#include "runtime/runtime_session.hpp"

#include <chrono>
#include <span>
#include <string_view>

#include <spawn.h>

#include <sys/types.h>

namespace howdy::pam::auth_helper_process {

	struct SpawnRequest {
		void  *context   = nullptr;
		pid_t *child_pid = nullptr;
		// Set to the child's pidfd when the spawn took one with the child (pidfd_spawn); left at
		// -1 otherwise, and the caller takes it after the spawn.
		int                              *child_pidfd = nullptr;
		const char                       *path        = nullptr;
		const posix_spawn_file_actions_t *actions     = nullptr;
		std::span<char *const>            argv;
		std::span<char *const>            envp;
	};

	using LogObserver = void (*)(void *context, std::string_view message);

	struct Operations {
		void *context = nullptr;

		int (*socketpair)(void *, int domain, int type, int protocol, int *socket_fds) = nullptr;
		int (*duplicate_fd)(void *, int fd, int minimum_fd)                            = nullptr;
		int (*actions_init)(void *, posix_spawn_file_actions_t *)                      = nullptr;
		int (*actions_addopen)(void *, posix_spawn_file_actions_t *, int fd, const char *path,
		                       int flags)                                              = nullptr;
		int (*actions_adddup2)(void *, posix_spawn_file_actions_t *, int source_fd,
		                       int target_fd)                                          = nullptr;
		int (*actions_addclosefrom)(void *, posix_spawn_file_actions_t *, int from_fd) = nullptr;
		int (*actions_destroy)(void *, posix_spawn_file_actions_t *)                   = nullptr;
		int (*spawn)(const SpawnRequest &)                                             = nullptr;
		int (*close)(void *, int fd)                                                   = nullptr;
		LogObserver log_observer                                                       = nullptr;
	};

	__attribute__((visibility("hidden"))) auto ProductionOperations() -> Operations;
	// Runs the setuid helper's `open` for `username` (the calling user) and receives its one
	// message (protocol/runtime_descriptor_message.hpp). The result comes only from that message
	// followed by end of file; the helper's exit status is reaped and logged, never used to
	// decide. On success `descriptors` owns the config and, when ready, the model descriptor.
	__attribute__((visibility("hidden"))) auto
	OpenRuntimeDescriptors(std::string_view username, RuntimeDescriptorSet *descriptors,
	                       const Operations                     &operations,
	                       std::chrono::steady_clock::time_point deadline) -> bool;
	__attribute__((visibility("hidden"))) auto
	OpenRuntimeDescriptors(std::string_view username, RuntimeDescriptorSet *descriptors,
	                       const Operations &operations) -> bool;
	__attribute__((visibility("hidden"))) auto
	OpenRuntimeDescriptors(std::string_view username, RuntimeDescriptorSet *descriptors) -> bool;

}  // namespace howdy::pam::auth_helper_process
