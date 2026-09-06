#ifndef _GNU_SOURCE
#	define _GNU_SOURCE
#endif

#include "internal.hpp"
#include "protocol/auth_helper_protocol.hpp"
#include "support/fd_io.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <paths.hpp>
#include <spawn.h>
#include <string>
#include <string_view>
#include <syslog.h>
#include <unistd.h>

#include <sys/socket.h>

namespace {

	using howdy::pam::auth_helper_process::Operations;
	using howdy::pam::auth_helper_process::SpawnRequest;
	using howdy::pam::auth_helper_process::internal::PreparedHelperSpawn;

	auto production_pipe2(void *context, int *pipe_fds, int flags) -> int {
		(void)context;
		return pipe2(pipe_fds, flags);
	}

	auto production_socketpair(void *context, int domain, int type, int protocol, int *socket_fds)
	    -> int {
		(void)context;
		return socketpair(domain, type, protocol, socket_fds);
	}

	auto production_duplicate_fd(void *context, int fd, int minimum_fd) -> int {
		(void)context;
		return fcntl(fd, F_DUPFD_CLOEXEC, minimum_fd);
	}

	auto production_actions_init(void *context, posix_spawn_file_actions_t *actions) -> int {
		(void)context;
		return posix_spawn_file_actions_init(actions);
	}

	auto production_actions_adddup2(void *context, posix_spawn_file_actions_t *actions,
	                                int source_fd, int target_fd) -> int {
		(void)context;
		return posix_spawn_file_actions_adddup2(actions, source_fd, target_fd);
	}

	auto production_actions_addclose(void *context, posix_spawn_file_actions_t *actions, int fd)
	    -> int {
		(void)context;
		return posix_spawn_file_actions_addclose(actions, fd);
	}

	auto production_actions_addclosefrom(void *context, posix_spawn_file_actions_t *actions,
	                                     int from_fd) -> int {
		(void)context;
		return posix_spawn_file_actions_addclosefrom_np(actions, from_fd);
	}

	auto production_actions_destroy(void *context, posix_spawn_file_actions_t *actions) -> int {
		(void)context;
		return posix_spawn_file_actions_destroy(actions);
	}

	auto production_spawn(const SpawnRequest &request) -> int {
		(void)request.context;
		return posix_spawn(request.child_pid, request.path, request.actions, nullptr, request.argv,
		                   request.envp);
	}

	auto production_close(void *context, int fd) -> int {
		(void)context;
		return close(fd);
	}

	auto production_read_bounded(void *context, howdy::native::BoundedReadRequest request)
	    -> howdy::native::BoundedReadResult {
		(void)context;
		return howdy::native::read_fd_to_string_bounded(request);
	}

	void write_helper_spawn_error_log(const Operations &operations, const char *operation,
	                                  int error_code) {
		syslog(LOG_ERR, "%s failed for auth helper: %s (%d)", operation, strerror(error_code),
		       error_code);
		if (operations.log_observer != nullptr) {
			const std::string message = std::string(operation) +
			                            " failed for auth helper: " + strerror(error_code) + " (" +
			                            std::to_string(error_code) + ")";
			operations.log_observer(operations.context, message);
		}
	}

	auto normalize_pipe_fds(const Operations &operations, std::array<int, 2> &pipe, int minimum_fd)
	    -> bool {
		for (int &pipe_fd : pipe) {
			if (pipe_fd >= minimum_fd) {
				continue;
			}
			const int normalized_fd =
			    operations.duplicate_fd(operations.context, pipe_fd, minimum_fd);
			if (normalized_fd < 0) {
				write_helper_spawn_error_log(operations, "fcntl(F_DUPFD_CLOEXEC)", errno);
				howdy::pam::auth_helper_process::internal::close_owned_fd(operations, pipe[0]);
				howdy::pam::auth_helper_process::internal::close_owned_fd(operations, pipe[1]);
				return false;
			}
			howdy::pam::auth_helper_process::internal::close_owned_fd(operations, pipe_fd);
			pipe_fd = normalized_fd;
		}
		return true;
	}

	void destroy_spawn_actions(const Operations &operations, posix_spawn_file_actions_t *actions) {
		const int result = operations.actions_destroy(operations.context, actions);
		if (result != 0) {
			write_helper_spawn_error_log(operations, "posix_spawn_file_actions_destroy", result);
		}
	}

	auto fail_spawn_setup(const Operations &operations, PreparedHelperSpawn *spawn,
	                      const char *operation, int error_code) -> bool {
		write_helper_spawn_error_log(operations, operation, error_code);
		destroy_spawn_actions(operations, &spawn->actions);
		howdy::pam::auth_helper_process::internal::close_owned_fd(operations,
		                                                          spawn->output_pipe[0]);
		howdy::pam::auth_helper_process::internal::close_owned_fd(operations,
		                                                          spawn->output_pipe[1]);
		howdy::pam::auth_helper_process::internal::close_owned_fd(operations,
		                                                          spawn->lease_socket[0]);
		howdy::pam::auth_helper_process::internal::close_owned_fd(operations,
		                                                          spawn->lease_socket[1]);
		return false;
	}

}  // namespace

namespace howdy::pam::auth_helper_process::internal {

	auto production_operations() -> Operations {
		return {
		    .pipe2                = production_pipe2,
		    .socketpair           = production_socketpair,
		    .duplicate_fd         = production_duplicate_fd,
		    .actions_init         = production_actions_init,
		    .actions_adddup2      = production_actions_adddup2,
		    .actions_addclose     = production_actions_addclose,
		    .actions_addclosefrom = production_actions_addclosefrom,
		    .actions_destroy      = production_actions_destroy,
		    .spawn                = production_spawn,
		    .close                = production_close,
		    .read_bounded         = production_read_bounded,
		};
	}

	void close_owned_fd(const Operations &operations, int &fd) {
		if (fd < 0) {
			return;
		}
		(void)operations.close(operations.context, fd);
		fd = -1;
	}

	auto setup_helper_spawn(const Operations &operations, PreparedHelperSpawn *spawn) -> bool {
		if (operations.pipe2(operations.context, spawn->output_pipe.data(), O_CLOEXEC) != 0) {
			syslog(LOG_ERR, "Failed to create auth helper pipe: %s (%d)", strerror(errno), errno);
			return false;
		}
		if (!normalize_pipe_fds(operations, spawn->output_pipe, STDERR_FILENO + 1)) {
			return false;
		}
		if (operations.socketpair(operations.context, AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0,
		                          spawn->lease_socket.data()) != 0) {
			syslog(LOG_ERR, "Failed to create auth helper lease socketpair: %s (%d)",
			       strerror(errno), errno);
			close_owned_fd(operations, spawn->output_pipe[0]);
			close_owned_fd(operations, spawn->output_pipe[1]);
			return false;
		}
		if (!normalize_pipe_fds(operations, spawn->lease_socket,
		                        howdy::native::auth_helper_protocol::kLeaseSocketFd + 1)) {
			close_owned_fd(operations, spawn->output_pipe[0]);
			close_owned_fd(operations, spawn->output_pipe[1]);
			return false;
		}
		const int init_result = operations.actions_init(operations.context, &spawn->actions);
		if (init_result != 0) {
			write_helper_spawn_error_log(operations, "posix_spawn_file_actions_init", init_result);
			close_owned_fd(operations, spawn->output_pipe[0]);
			close_owned_fd(operations, spawn->output_pipe[1]);
			close_owned_fd(operations, spawn->lease_socket[0]);
			close_owned_fd(operations, spawn->lease_socket[1]);
			return false;
		}

		int result =
		    operations.actions_addclose(operations.context, &spawn->actions, spawn->output_pipe[0]);
		if (result != 0) {
			return fail_spawn_setup(operations, spawn,
			                        "posix_spawn_file_actions_addclose(pipe read end)", result);
		}
		result = operations.actions_adddup2(operations.context, &spawn->actions,
		                                    spawn->output_pipe[1], STDOUT_FILENO);
		if (result != 0) {
			return fail_spawn_setup(operations, spawn,
			                        "posix_spawn_file_actions_adddup2(STDOUT_FILENO)", result);
		}
		result = operations.actions_adddup2(operations.context, &spawn->actions,
		                                    spawn->output_pipe[1], STDERR_FILENO);
		if (result != 0) {
			return fail_spawn_setup(operations, spawn,
			                        "posix_spawn_file_actions_adddup2(STDERR_FILENO)", result);
		}
		if (spawn->output_pipe[1] != STDOUT_FILENO && spawn->output_pipe[1] != STDERR_FILENO) {
			result = operations.actions_addclose(operations.context, &spawn->actions,
			                                     spawn->output_pipe[1]);
			if (result != 0) {
				return fail_spawn_setup(
				    operations, spawn, "posix_spawn_file_actions_addclose(pipe write end)", result);
			}
		}
		result = operations.actions_addclose(operations.context, &spawn->actions,
		                                     spawn->lease_socket[0]);
		if (result != 0) {
			return fail_spawn_setup(operations, spawn,
			                        "posix_spawn_file_actions_addclose(lease parent end)", result);
		}
		result =
		    operations.actions_adddup2(operations.context, &spawn->actions, spawn->lease_socket[1],
		                               howdy::native::auth_helper_protocol::kLeaseSocketFd);
		if (result != 0) {
			return fail_spawn_setup(operations, spawn, "posix_spawn_file_actions_adddup2(lease fd)",
			                        result);
		}
		if (spawn->lease_socket[1] != howdy::native::auth_helper_protocol::kLeaseSocketFd) {
			result = operations.actions_addclose(operations.context, &spawn->actions,
			                                     spawn->lease_socket[1]);
			if (result != 0) {
				return fail_spawn_setup(operations, spawn,
				                        "posix_spawn_file_actions_addclose(lease child end)",
				                        result);
			}
		}
		result = operations.actions_addclosefrom(
		    operations.context, &spawn->actions,
		    howdy::native::auth_helper_protocol::kLeaseSocketFd + 1);
		if (result != 0) {
			return fail_spawn_setup(operations, spawn, "posix_spawn_file_actions_addclosefrom",
			                        result);
		}
		return true;
	}

	auto spawn_prepare_helper(std::string_view username, const Operations &operations,
	                          PreparedHelperSpawn *spawn, pid_t *child_pid) -> bool {
		std::string           username_string(username);
		std::array<char *, 4> args   = {const_cast<char *>(kAuthHelperPath),
		                                const_cast<char *>("prepare"), username_string.data(),
		                                nullptr};
		std::array<char *, 1> env    = {nullptr};
		const int             result = operations.spawn({.context   = operations.context,
		                                                 .child_pid = child_pid,
		                                                 .path      = kAuthHelperPath,
		                                                 .actions   = &spawn->actions,
		                                                 .argv      = args.data(),
		                                                 .envp      = env.data()});
		if (result != 0) {
			write_helper_spawn_error_log(operations, "posix_spawn", result);
		}
		destroy_spawn_actions(operations, &spawn->actions);
		close_owned_fd(operations, spawn->output_pipe[1]);
		close_owned_fd(operations, spawn->lease_socket[1]);
		if (result != 0) {
			close_owned_fd(operations, spawn->output_pipe[0]);
			close_owned_fd(operations, spawn->lease_socket[0]);
			return false;
		}
		return true;
	}

}  // namespace howdy::pam::auth_helper_process::internal
