#ifndef _GNU_SOURCE
#	define _GNU_SOURCE
#endif

#include "internal.hpp"
#include "protocol/auth_helper_protocol.hpp"
#include "runtime/auth_helper_process.hpp"
#include "support/error_message.hpp"
#include "support/fd_io.hpp"

#include <array>
#include <cerrno>
#include <paths.hpp>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <spawn.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/syslog.h>
#include <sys/types.h>

namespace {

	using AuthHelperOperations   = howdy::pam::auth_helper_process::Operations;
	using AuthHelperSpawnRequest = howdy::pam::auth_helper_process::SpawnRequest;
	using howdy::pam::auth_helper_process::internal::PreparedHelperSpawn;
	using howdy::pam::auth_helper_process::internal::ProcessChannel;

	auto ProductionPipe2(void *context, int *pipe_fds, int flags) -> int {
		(void)context;
		return pipe2(pipe_fds, flags);
	}

	auto ProductionSocketpair(void *context, int domain, int type, int protocol, int *socket_fds)
	    -> int {
		(void)context;
		return socketpair(domain, type, protocol, socket_fds);
	}

	auto ProductionDuplicateFd(void *context, int fd, int minimum_fd) -> int {
		(void)context;
		return fcntl(fd, F_DUPFD_CLOEXEC, minimum_fd);
	}

	auto ProductionActionsInit(void *context, posix_spawn_file_actions_t *actions) -> int {
		(void)context;
		return posix_spawn_file_actions_init(actions);
	}

	auto ProductionActionsAdddup2(void *context, posix_spawn_file_actions_t *actions, int source_fd,
	                              int target_fd) -> int {
		(void)context;
		return posix_spawn_file_actions_adddup2(actions, source_fd, target_fd);
	}

	auto ProductionActionsAddclose(void *context, posix_spawn_file_actions_t *actions, int fd)
	    -> int {
		(void)context;
		return posix_spawn_file_actions_addclose(actions, fd);
	}

	auto ProductionActionsAddclosefrom(void *context, posix_spawn_file_actions_t *actions,
	                                   int from_fd) -> int {
		(void)context;
		return posix_spawn_file_actions_addclosefrom_np(actions, from_fd);
	}

	auto ProductionActionsDestroy(void *context, posix_spawn_file_actions_t *actions) -> int {
		(void)context;
		return posix_spawn_file_actions_destroy(actions);
	}

	auto ProductionSpawn(const AuthHelperSpawnRequest &request) -> int {
		(void)request.context;
		return posix_spawn(request.child_pid, request.path, request.actions, nullptr,
		                   request.argv.data(), request.envp.data());
	}

	auto ProductionClose(void *context, int fd) -> int {
		(void)context;
		return close(fd);
	}

	auto ProductionReadBounded(void *context, howdy::native::BoundedReadRequest request)
	    -> howdy::native::BoundedReadResult {
		(void)context;
		return howdy::native::ReadFdToStringBounded(request);
	}

	void WriteHelperSpawnErrorLog(const AuthHelperOperations &operations, const char *operation,
	                              int error_code) noexcept {
		try {
			const std::string error_message = howdy::native::SystemErrorMessage(error_code);
			syslog(LOG_ERR, "%s failed for auth helper: %s (%d)", operation, error_message.c_str(),
			       error_code);
			if (operations.log_observer != nullptr) {
				const std::string message = std::string(operation) +
				                            " failed for auth helper: " + error_message + " (" +
				                            std::to_string(error_code) + ")";
				operations.log_observer(operations.context, message);
			}
		} catch (...) {
			syslog(LOG_ERR, "%s failed for auth helper (%d)", operation, error_code);
		}
	}

	auto NormalizePipeFds(const AuthHelperOperations &operations, ProcessChannel &channel,
	                      int minimum_fd) -> bool {
		for (int &pipe_fd : channel.Descriptors()) {
			if (pipe_fd >= minimum_fd) {
				continue;
			}
			const int normalized_fd =
			    operations.duplicate_fd(operations.context, pipe_fd, minimum_fd);
			if (normalized_fd < 0) {
				WriteHelperSpawnErrorLog(operations, "fcntl(F_DUPFD_CLOEXEC)", errno);
				howdy::pam::auth_helper_process::internal::CloseOwnedFd(operations,
				                                                        channel.Parent());
				howdy::pam::auth_helper_process::internal::CloseOwnedFd(operations,
				                                                        channel.Child());
				return false;
			}
			howdy::pam::auth_helper_process::internal::CloseOwnedFd(operations, pipe_fd);
			pipe_fd = normalized_fd;
		}
		return true;
	}

	void DestroySpawnActions(const AuthHelperOperations &operations,
	                         posix_spawn_file_actions_t *actions) {
		const int result = operations.actions_destroy(operations.context, actions);
		if (result != 0) {
			WriteHelperSpawnErrorLog(operations, "posix_spawn_file_actions_destroy", result);
		}
	}

	auto FailSpawnSetup(const AuthHelperOperations &operations, PreparedHelperSpawn *spawn,
	                    const char *operation, int error_code) -> bool {
		WriteHelperSpawnErrorLog(operations, operation, error_code);
		DestroySpawnActions(operations, &spawn->actions);
		howdy::pam::auth_helper_process::internal::CloseOwnedFd(operations,
		                                                        spawn->output_pipe.Parent());
		howdy::pam::auth_helper_process::internal::CloseOwnedFd(operations,
		                                                        spawn->output_pipe.Child());
		howdy::pam::auth_helper_process::internal::CloseOwnedFd(operations,
		                                                        spawn->lease_socket.Parent());
		howdy::pam::auth_helper_process::internal::CloseOwnedFd(operations,
		                                                        spawn->lease_socket.Child());
		return false;
	}

}  // namespace

namespace howdy::pam::auth_helper_process::internal {

	auto ProductionOperations() -> AuthHelperOperations {
		return {
		    .pipe2                = ProductionPipe2,
		    .socketpair           = ProductionSocketpair,
		    .duplicate_fd         = ProductionDuplicateFd,
		    .actions_init         = ProductionActionsInit,
		    .actions_adddup2      = ProductionActionsAdddup2,
		    .actions_addclose     = ProductionActionsAddclose,
		    .actions_addclosefrom = ProductionActionsAddclosefrom,
		    .actions_destroy      = ProductionActionsDestroy,
		    .spawn                = ProductionSpawn,
		    .close                = ProductionClose,
		    .read_bounded         = ProductionReadBounded,
		};
	}

	void CloseOwnedFd(const AuthHelperOperations &operations, int &fd) {
		if (fd < 0) {
			return;
		}
		(void)operations.close(operations.context, fd);
		fd = -1;
	}

	auto SetupHelperSpawn(const AuthHelperOperations &operations, PreparedHelperSpawn *spawn)
	    -> bool {
		if (operations.pipe2(operations.context, spawn->output_pipe.Data(), O_CLOEXEC) != 0) {
			WriteHelperSpawnErrorLog(operations, "pipe2", errno);
			return false;
		}
		if (!NormalizePipeFds(operations, spawn->output_pipe, STDERR_FILENO + 1)) {
			return false;
		}
		if (operations.socketpair(operations.context, AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0,
		                          spawn->lease_socket.Data()) != 0) {
			WriteHelperSpawnErrorLog(operations, "socketpair", errno);
			CloseOwnedFd(operations, spawn->output_pipe.Parent());
			CloseOwnedFd(operations, spawn->output_pipe.Child());
			return false;
		}
		if (!NormalizePipeFds(operations, spawn->lease_socket,
		                      howdy::native::auth_helper_protocol::kLeaseSocketFd + 1)) {
			CloseOwnedFd(operations, spawn->output_pipe.Parent());
			CloseOwnedFd(operations, spawn->output_pipe.Child());
			return false;
		}
		const int init_result = operations.actions_init(operations.context, &spawn->actions);
		if (init_result != 0) {
			WriteHelperSpawnErrorLog(operations, "posix_spawn_file_actions_init", init_result);
			CloseOwnedFd(operations, spawn->output_pipe.Parent());
			CloseOwnedFd(operations, spawn->output_pipe.Child());
			CloseOwnedFd(operations, spawn->lease_socket.Parent());
			CloseOwnedFd(operations, spawn->lease_socket.Child());
			return false;
		}

		int result = operations.actions_addclose(operations.context, &spawn->actions,
		                                         spawn->output_pipe.Parent());
		if (result != 0) {
			return FailSpawnSetup(operations, spawn,
			                      "posix_spawn_file_actions_addclose(pipe read end)", result);
		}
		result = operations.actions_adddup2(operations.context, &spawn->actions,
		                                    spawn->output_pipe.Child(), STDOUT_FILENO);
		if (result != 0) {
			return FailSpawnSetup(operations, spawn,
			                      "posix_spawn_file_actions_adddup2(STDOUT_FILENO)", result);
		}
		result = operations.actions_adddup2(operations.context, &spawn->actions,
		                                    spawn->output_pipe.Child(), STDERR_FILENO);
		if (result != 0) {
			return FailSpawnSetup(operations, spawn,
			                      "posix_spawn_file_actions_adddup2(STDERR_FILENO)", result);
		}
		if (spawn->output_pipe.Child() != STDOUT_FILENO &&
		    spawn->output_pipe.Child() != STDERR_FILENO) {
			result = operations.actions_addclose(operations.context, &spawn->actions,
			                                     spawn->output_pipe.Child());
			if (result != 0) {
				return FailSpawnSetup(operations, spawn,
				                      "posix_spawn_file_actions_addclose(pipe write end)", result);
			}
		}
		result = operations.actions_addclose(operations.context, &spawn->actions,
		                                     spawn->lease_socket.Parent());
		if (result != 0) {
			return FailSpawnSetup(operations, spawn,
			                      "posix_spawn_file_actions_addclose(lease parent end)", result);
		}
		result = operations.actions_adddup2(operations.context, &spawn->actions,
		                                    spawn->lease_socket.Child(),
		                                    howdy::native::auth_helper_protocol::kLeaseSocketFd);
		if (result != 0) {
			return FailSpawnSetup(operations, spawn, "posix_spawn_file_actions_adddup2(lease fd)",
			                      result);
		}
		if (spawn->lease_socket.Child() != howdy::native::auth_helper_protocol::kLeaseSocketFd) {
			result = operations.actions_addclose(operations.context, &spawn->actions,
			                                     spawn->lease_socket.Child());
			if (result != 0) {
				return FailSpawnSetup(operations, spawn,
				                      "posix_spawn_file_actions_addclose(lease child end)", result);
			}
		}
		result = operations.actions_addclosefrom(
		    operations.context, &spawn->actions,
		    howdy::native::auth_helper_protocol::kLeaseSocketFd + 1);
		if (result != 0) {
			return FailSpawnSetup(operations, spawn, "posix_spawn_file_actions_addclosefrom",
			                      result);
		}
		return true;
	}

	auto SpawnPrepareHelper(std::string_view username, const AuthHelperOperations &operations,
	                        PreparedHelperSpawn *spawn, pid_t *child_pid) -> bool {
		std::string           helper_path(kAuthHelperPath);
		std::string           command("prepare");
		std::string           username_string(username);
		std::array<char *, 4> args   = {helper_path.data(), command.data(), username_string.data(),
		                                nullptr};
		std::array<char *, 1> env    = {nullptr};
		const int             result = operations.spawn({.context   = operations.context,
		                                                 .child_pid = child_pid,
		                                                 .path      = kAuthHelperPath,
		                                                 .actions   = &spawn->actions,
		                                                 .argv      = args,
		                                                 .envp      = env});
		if (result != 0) {
			WriteHelperSpawnErrorLog(operations, "posix_spawn", result);
		}
		DestroySpawnActions(operations, &spawn->actions);
		CloseOwnedFd(operations, spawn->output_pipe.Child());
		CloseOwnedFd(operations, spawn->lease_socket.Child());
		if (result != 0) {
			CloseOwnedFd(operations, spawn->output_pipe.Parent());
			CloseOwnedFd(operations, spawn->lease_socket.Parent());
			return false;
		}
		return true;
	}

}  // namespace howdy::pam::auth_helper_process::internal
