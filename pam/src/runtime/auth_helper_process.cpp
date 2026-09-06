#ifndef _GNU_SOURCE
#	define _GNU_SOURCE
#endif

#include "runtime/auth_helper_process.hpp"

#include "protocol/auth_helper_protocol.hpp"
#include "protocol/compare_exit.hpp"
#include "runtime/runtime_session.hpp"
#include "storage/staged_runtime_policy.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <paths.hpp>
#include <poll.h>
#include <spawn.h>
#include <string>
#include <string_view>
#include <syslog.h>
#include <unistd.h>

#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>

namespace {

	using howdy::native::CompareExit;

	constexpr std::size_t kAuthHelperOutputLimit  = 9216;
	constexpr auto        kAuthHelperTimeout      = std::chrono::seconds(10);
	constexpr auto        kHelperWaitPollInterval = std::chrono::milliseconds(10);

	using AuthHelperOutput          = howdy::pam::auth_helper_process::Output;
	using AuthHelperSpawnOperations = howdy::pam::auth_helper_process::Operations;
	using AuthHelperSpawnRequest    = howdy::pam::auth_helper_process::SpawnRequest;

	void log_auth_helper_read_error(int error_number) {
		syslog(LOG_ERR, "Failed to read auth helper output: %s (%d)", strerror(error_number),
		       error_number);
	}

	auto make_wait_exit_status(CompareExit exit_code) -> int {
		return static_cast<int>(exit_code) << 8;
	}

	auto set_required_helper_output_value(bool *seen, std::string *target, const std::string &value)
	    -> bool {
		if (*seen || value.empty()) {
			return false;
		}
		*seen   = true;
		*target = value;
		return true;
	}

	auto parse_auth_helper_output(const std::string &output) -> AuthHelperOutput {
		AuthHelperOutput result;
		bool             saw_config_path     = false;
		bool             saw_user_models_dir = false;

		std::size_t offset = 0;
		while (offset < output.size()) {
			const auto next      = output.find('\n', offset);
			const auto end       = next == std::string::npos ? output.size() : next;
			const auto line      = output.substr(offset, end - offset);
			const auto separator = line.find('=');

			if (separator == std::string::npos) {
				return result;
			}

			const auto key   = line.substr(0, separator);
			const auto value = line.substr(separator + 1);
			if (key == howdy::native::auth_helper_protocol::kConfigPathKey) {
				if (!set_required_helper_output_value(&saw_config_path, &result.config_path,
				                                      value)) {
					return result;
				}
			} else if (key == howdy::native::auth_helper_protocol::kUserModelsDirKey) {
				if (!set_required_helper_output_value(&saw_user_models_dir, &result.user_models_dir,
				                                      value)) {
					return result;
				}
			} else {
				return result;
			}

			if (next == std::string::npos) {
				break;
			}
			offset = next + 1;
		}

		result.valid = saw_config_path && saw_user_models_dir;
		if (!result.valid) {
			return result;
		}

		const std::filesystem::path config_path(result.config_path);
		const std::filesystem::path user_models_dir(result.user_models_dir);
		result.valid = howdy::native::auth_helper_protocol::matches_prepared_runtime_layout(
		    config_path.parent_path(), config_path, user_models_dir, getuid());
		return result;
	}

	auto wait_for_helper_process(pid_t child_pid) -> int {
		while (true) {
			int         status      = 0;
			const pid_t wait_result = waitpid(child_pid, &status, 0);
			if (wait_result == child_pid) {
				return status;
			}
			if (wait_result < 0 && errno == EINTR) {
				continue;
			}
			return make_wait_exit_status(CompareExit::kAbort);
		}
	}

	using HelperDeadline = std::chrono::steady_clock::time_point;

	auto deadline_poll_timeout(HelperDeadline deadline) -> int {
		const auto remaining = deadline - std::chrono::steady_clock::now();
		if (remaining <= HelperDeadline::duration::zero()) {
			return 0;
		}
		return static_cast<int>(std::chrono::ceil<std::chrono::milliseconds>(remaining).count());
	}

	auto terminate_and_reap_helper_process(pid_t child_pid) -> int {
		int         status              = 0;
		const pid_t initial_wait_result = waitpid(child_pid, &status, WNOHANG);
		if (initial_wait_result == child_pid) {
			return status;
		}
		if (initial_wait_result < 0 && errno == ECHILD) {
			return make_wait_exit_status(CompareExit::kAbort);
		}

		if (kill(child_pid, SIGTERM) != 0 && errno != ESRCH) {
			syslog(LOG_WARNING, "Failed to terminate auth helper process: %s (%d)", strerror(errno),
			       errno);
		}

		for (int attempts = 0; attempts < 50; ++attempts) {
			status                  = 0;
			const pid_t wait_result = waitpid(child_pid, &status, WNOHANG);
			if (wait_result == child_pid) {
				return status;
			}
			if (wait_result < 0) {
				if (errno == EINTR) {
					continue;
				}
				return make_wait_exit_status(CompareExit::kAbort);
			}
			usleep(10000);
		}

		if (kill(child_pid, SIGKILL) != 0 && errno != ESRCH) {
			syslog(LOG_WARNING, "Failed to kill auth helper process: %s (%d)", strerror(errno),
			       errno);
		}
		return wait_for_helper_process(child_pid);
	}

	enum class HelperWaitResult : std::uint8_t {
		kExited,
		kTimedOut,
		kWaitError,
	};

	auto wait_for_helper_process_until(pid_t child_pid, HelperDeadline deadline, int *status)
	    -> HelperWaitResult {
		while (true) {
			const pid_t wait_result = waitpid(child_pid, status, WNOHANG);
			if (wait_result == child_pid) {
				return std::chrono::steady_clock::now() >= deadline ? HelperWaitResult::kTimedOut
				                                                    : HelperWaitResult::kExited;
			}
			if (wait_result < 0) {
				if (errno == EINTR) {
					continue;
				}
				if (errno != ECHILD) {
					(void)terminate_and_reap_helper_process(child_pid);
				}
				return HelperWaitResult::kWaitError;
			}
			if (std::chrono::steady_clock::now() >= deadline) {
				(void)terminate_and_reap_helper_process(child_pid);
				return HelperWaitResult::kTimedOut;
			}
			const auto wake_time =
			    std::min(deadline, std::chrono::steady_clock::now() + kHelperWaitPollInterval);
			(void)poll(nullptr, 0, deadline_poll_timeout(wake_time));
		}
	}

	enum class HelperReadResult : std::uint8_t {
		kComplete,
		kTimedOut,
		kReadError,
		kOutputLimit,
	};

	enum class HelperPollResult : std::uint8_t {
		kReady,
		kRetry,
		kTimedOut,
		kError,
	};

	auto poll_auth_helper_output(int output_fd, HelperDeadline deadline) -> HelperPollResult {
		pollfd    descriptor{.fd = output_fd, .events = POLLIN, .revents = 0};
		const int poll_result = poll(&descriptor, 1, deadline_poll_timeout(deadline));
		if (poll_result < 0) {
			if (errno == EINTR) {
				return HelperPollResult::kRetry;
			}
			const int read_error = errno;
			log_auth_helper_read_error(read_error);
			return HelperPollResult::kError;
		}
		if (poll_result == 0) {
			return HelperPollResult::kTimedOut;
		}
		if ((descriptor.revents & (POLLNVAL | POLLERR)) != 0) {
			const int read_error = (descriptor.revents & POLLNVAL) != 0 ? EBADF : EIO;
			log_auth_helper_read_error(read_error);
			return HelperPollResult::kError;
		}
		return (descriptor.revents & (POLLIN | POLLHUP)) == 0 ? HelperPollResult::kRetry
		                                                      : HelperPollResult::kReady;
	}

	auto read_auth_helper_output_from_fd(int output_fd, std::string &output,
	                                     HelperDeadline deadline) -> HelperReadResult {
		if (output_fd < 0) {
			constexpr int read_error = EBADF;
			log_auth_helper_read_error(read_error);
			return HelperReadResult::kReadError;
		}

		std::array<char, 4096> buffer{};
		while (true) {
			const auto poll_result = poll_auth_helper_output(output_fd, deadline);
			if (poll_result == HelperPollResult::kRetry) {
				continue;
			}
			if (poll_result == HelperPollResult::kTimedOut) {
				output.clear();
				return HelperReadResult::kTimedOut;
			}
			if (poll_result == HelperPollResult::kError) {
				output.clear();
				return HelperReadResult::kReadError;
			}

			const ssize_t bytes_read = read(output_fd, buffer.data(), buffer.size());
			if (bytes_read > 0) {
				if (output.size() + static_cast<std::size_t>(bytes_read) >=
				    kAuthHelperOutputLimit) {
					output.clear();
					return HelperReadResult::kOutputLimit;
				}
				output.append(buffer.data(), static_cast<std::size_t>(bytes_read));
				continue;
			}
			if (bytes_read == 0) {
				if (std::chrono::steady_clock::now() >= deadline) {
					output.clear();
					return HelperReadResult::kTimedOut;
				}
				return HelperReadResult::kComplete;
			}
			if (errno != EINTR && errno != EAGAIN) {
				const int read_error = errno;
				output.clear();
				log_auth_helper_read_error(read_error);
				return HelperReadResult::kReadError;
			}
		}
	}

	auto read_auth_helper_output_until(int output_fd, std::string *output,
	                                   const AuthHelperSpawnOperations &operations,
	                                   HelperDeadline deadline) -> HelperReadResult {
		if (output == nullptr) {
			return HelperReadResult::kReadError;
		}
		output->clear();

		if (operations.read_bounded != nullptr) {
			const auto helper_output = operations.read_bounded(
			    operations.context, {.fd = output_fd, .max_bytes = kAuthHelperOutputLimit});
			if (helper_output.read_error) {
				log_auth_helper_read_error(helper_output.error_number);
				return HelperReadResult::kReadError;
			}
			if (helper_output.hit_limit) {
				return HelperReadResult::kOutputLimit;
			}
			if (std::chrono::steady_clock::now() >= deadline) {
				output->clear();
				return HelperReadResult::kTimedOut;
			}
			*output = helper_output.output;
			return HelperReadResult::kComplete;
		}

		return read_auth_helper_output_from_fd(output_fd, *output, deadline);
	}

	void capture_auth_helper_log(const AuthHelperSpawnOperations &operations,
	                             std::string_view                 message) {
		if (operations.log_observer != nullptr) {
			operations.log_observer(operations.context, message);
		}
	}

	void log_prepare_timeout(const AuthHelperSpawnOperations &operations) {
		constexpr std::string_view message = "Howdy auth helper prepare timed out";
		syslog(LOG_ERR, "%.*s", static_cast<int>(message.size()), message.data());
		capture_auth_helper_log(operations, message);
	}

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

	auto production_spawn(const AuthHelperSpawnRequest &request) -> int {
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

	auto production_auth_helper_spawn_operations() -> AuthHelperSpawnOperations {
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

	void write_helper_spawn_error_log(const AuthHelperSpawnOperations &operations,
	                                  const char *operation, int error_code) {
		syslog(LOG_ERR, "%s failed for auth helper: %s (%d)", operation, strerror(error_code),
		       error_code);
		if (operations.log_observer != nullptr) {
			const std::string message = std::string(operation) +
			                            " failed for auth helper: " + strerror(error_code) + " (" +
			                            std::to_string(error_code) + ")";
			operations.log_observer(operations.context, message);
		}
	}

	void close_owned_pipe_fd(const AuthHelperSpawnOperations &operations, int &fd) {
		if (fd < 0) {
			return;
		}
		(void)operations.close(operations.context, fd);
		fd = -1;
	}

	auto normalize_pipe_fds(const AuthHelperSpawnOperations &operations, std::array<int, 2> &pipe,
	                        int minimum_fd) -> bool {
		for (int &pipe_fd : pipe) {
			if (pipe_fd >= minimum_fd) {
				continue;
			}
			const int normalized_fd =
			    operations.duplicate_fd(operations.context, pipe_fd, minimum_fd);
			if (normalized_fd < 0) {
				write_helper_spawn_error_log(operations, "fcntl(F_DUPFD_CLOEXEC)", errno);
				close_owned_pipe_fd(operations, pipe[0]);
				close_owned_pipe_fd(operations, pipe[1]);
				return false;
			}
			close_owned_pipe_fd(operations, pipe_fd);
			pipe_fd = normalized_fd;
		}
		return true;
	}

	struct PreparedHelperSpawn {
		std::array<int, 2>         output_pipe  = {-1, -1};
		std::array<int, 2>         lease_socket = {-1, -1};
		posix_spawn_file_actions_t actions{};
	};

	void destroy_spawn_actions(const AuthHelperSpawnOperations &operations,
	                           posix_spawn_file_actions_t      *actions) {
		const int result = operations.actions_destroy(operations.context, actions);
		if (result != 0) {
			write_helper_spawn_error_log(operations, "posix_spawn_file_actions_destroy", result);
		}
	}

	auto fail_spawn_setup(const AuthHelperSpawnOperations &operations, PreparedHelperSpawn *spawn,
	                      const char *operation, int error_code) -> bool {
		write_helper_spawn_error_log(operations, operation, error_code);
		destroy_spawn_actions(operations, &spawn->actions);
		close_owned_pipe_fd(operations, spawn->output_pipe[0]);
		close_owned_pipe_fd(operations, spawn->output_pipe[1]);
		close_owned_pipe_fd(operations, spawn->lease_socket[0]);
		close_owned_pipe_fd(operations, spawn->lease_socket[1]);
		return false;
	}

	auto setup_helper_spawn(const AuthHelperSpawnOperations &operations, PreparedHelperSpawn *spawn)
	    -> bool {
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
			close_owned_pipe_fd(operations, spawn->output_pipe[0]);
			close_owned_pipe_fd(operations, spawn->output_pipe[1]);
			return false;
		}
		if (!normalize_pipe_fds(operations, spawn->lease_socket,
		                        howdy::native::auth_helper_protocol::kLeaseSocketFd + 1)) {
			close_owned_pipe_fd(operations, spawn->output_pipe[0]);
			close_owned_pipe_fd(operations, spawn->output_pipe[1]);
			return false;
		}
		const int init_result = operations.actions_init(operations.context, &spawn->actions);
		if (init_result != 0) {
			write_helper_spawn_error_log(operations, "posix_spawn_file_actions_init", init_result);
			close_owned_pipe_fd(operations, spawn->output_pipe[0]);
			close_owned_pipe_fd(operations, spawn->output_pipe[1]);
			close_owned_pipe_fd(operations, spawn->lease_socket[0]);
			close_owned_pipe_fd(operations, spawn->lease_socket[1]);
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

	auto spawn_prepare_helper(std::string_view                 username,
	                          const AuthHelperSpawnOperations &operations,
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
		close_owned_pipe_fd(operations, spawn->output_pipe[1]);
		close_owned_pipe_fd(operations, spawn->lease_socket[1]);
		if (result != 0) {
			close_owned_pipe_fd(operations, spawn->output_pipe[0]);
			close_owned_pipe_fd(operations, spawn->lease_socket[0]);
			return false;
		}
		return true;
	}

	enum class LeaseReceiveResult : std::uint8_t {
		kReceived,
		kRetry,
		kInvalid,
	};

	void close_control_descriptors(msghdr *message) {
		for (cmsghdr *control = CMSG_FIRSTHDR(message); control != nullptr;
		     control          = CMSG_NXTHDR(message, control)) {
			if (control->cmsg_level != SOL_SOCKET || control->cmsg_type != SCM_RIGHTS ||
			    control->cmsg_len < CMSG_LEN(0)) {
				continue;
			}
			const std::size_t count = (control->cmsg_len - CMSG_LEN(0)) / sizeof(int);
			for (std::size_t index = 0; index < count; ++index) {
				int descriptor = -1;
				std::memcpy(&descriptor, CMSG_DATA(control) + (index * sizeof(int)), sizeof(int));
				if (descriptor >= 0) {
					(void)close(descriptor);
				}
			}
		}
	}

	auto receive_lease_descriptor_once(int socket_fd, int *lease_fd) -> LeaseReceiveResult {
		char  marker = 0;
		iovec data{.iov_base = &marker, .iov_len = sizeof(marker)};
		alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(int) * 4) + CMSG_SPACE(sizeof(ucred))>
		       control_buffer{};
		msghdr message{};
		message.msg_iov        = &data;
		message.msg_iovlen     = 1;
		message.msg_control    = control_buffer.data();
		message.msg_controllen = control_buffer.size();

		const ssize_t received = recvmsg(socket_fd, &message, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
		if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
			return LeaseReceiveResult::kRetry;
		}
		if (received != 1 || marker != 'L' || (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0) {
			close_control_descriptors(&message);
			return LeaseReceiveResult::kInvalid;
		}

		cmsghdr *control = CMSG_FIRSTHDR(&message);
		if (control == nullptr || control->cmsg_level != SOL_SOCKET ||
		    control->cmsg_type != SCM_RIGHTS || control->cmsg_len != CMSG_LEN(sizeof(int)) ||
		    CMSG_NXTHDR(&message, control) != nullptr) {
			close_control_descriptors(&message);
			return LeaseReceiveResult::kInvalid;
		}

		int descriptor = -1;
		std::memcpy(&descriptor, CMSG_DATA(control), sizeof(descriptor));
		const int descriptor_flags = descriptor >= 0 ? fcntl(descriptor, F_GETFD) : -1;
		if (descriptor < 0 || descriptor_flags < 0 || (descriptor_flags & FD_CLOEXEC) == 0) {
			if (descriptor >= 0) {
				(void)close(descriptor);
			}
			return LeaseReceiveResult::kInvalid;
		}

		*lease_fd = descriptor;
		return LeaseReceiveResult::kReceived;
	}

	auto receive_lease_descriptor_until(int socket_fd, int *lease_fd, HelperDeadline deadline)
	    -> bool {
		if (socket_fd < 0 || lease_fd == nullptr) {
			return false;
		}
		*lease_fd = -1;
		while (true) {
			pollfd    descriptor{.fd = socket_fd, .events = POLLIN, .revents = 0};
			const int result = poll(&descriptor, 1, deadline_poll_timeout(deadline));
			if (result < 0 && errno == EINTR) {
				continue;
			}
			if (result <= 0 || (descriptor.revents & (POLLERR | POLLNVAL)) != 0) {
				return false;
			}
			const auto receive_result = receive_lease_descriptor_once(socket_fd, lease_fd);
			if (receive_result == LeaseReceiveResult::kRetry) {
				continue;
			}
			return receive_result == LeaseReceiveResult::kReceived;
		}
	}

	auto lease_socket_has_clean_eof(int socket_fd) -> bool {
		char  byte = 0;
		iovec data{.iov_base = &byte, .iov_len = sizeof(byte)};
		alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(int) * 4) + CMSG_SPACE(sizeof(ucred))>
		       control_buffer{};
		msghdr message{};
		message.msg_iov        = &data;
		message.msg_iovlen     = 1;
		message.msg_control    = control_buffer.data();
		message.msg_controllen = control_buffer.size();
		const ssize_t received = recvmsg(socket_fd, &message, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
		if (received != 0) {
			if (received > 0) {
				close_control_descriptors(&message);
			}
			return false;
		}
		return CMSG_FIRSTHDR(&message) == nullptr;
	}

	auto validate_lease_descriptor_impl(int lease_fd, const std::filesystem::path &root_dir,
	                                    uid_t owner_uid) -> bool {
		if (lease_fd < 0 || root_dir.empty() || root_dir.filename().empty()) {
			return false;
		}

		const int  flags = fcntl(lease_fd, F_GETFL);
		const auto policy =
		    howdy::native::staged_runtime_policy(howdy::native::StagedRuntimeRole::kLock);
		struct stat lease_stat{};
		if (flags < 0 || (flags & O_ACCMODE) != O_RDONLY || !policy.exact_link_count.has_value() ||
		    fstat(lease_fd, &lease_stat) != 0 || !S_ISREG(lease_stat.st_mode) ||
		    lease_stat.st_uid != owner_uid || lease_stat.st_nlink != *policy.exact_link_count ||
		    (lease_stat.st_mode & 07777) != policy.mode) {
			return false;
		}

		const int parent_fd =
		    open(root_dir.parent_path().c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
		if (parent_fd < 0) {
			return false;
		}
		const std::string lock_name = root_dir.filename().string() + ".lock";
		struct stat       path_stat{};
		const bool        identity_ok =
		    fstatat(parent_fd, lock_name.c_str(), &path_stat, AT_SYMLINK_NOFOLLOW) == 0 &&
		    S_ISREG(path_stat.st_mode) && path_stat.st_uid == owner_uid &&
		    path_stat.st_nlink == *policy.exact_link_count &&
		    (path_stat.st_mode & 07777) == policy.mode && path_stat.st_dev == lease_stat.st_dev &&
		    path_stat.st_ino == lease_stat.st_ino;
		(void)close(parent_fd);
		return identity_ok && flock(lease_fd, LOCK_SH | LOCK_NB) == 0;
	}

	enum class PrepareIoResult : std::uint8_t {
		kPending,
		kComplete,
		kFailed,
	};

	auto read_prepare_output_once(int output_fd, std::string *output) -> PrepareIoResult {
		std::array<char, 4096> buffer{};
		const ssize_t          size = read(output_fd, buffer.data(), buffer.size());
		if (size > 0) {
			if (output->size() + static_cast<std::size_t>(size) >= kAuthHelperOutputLimit) {
				syslog(LOG_ERR, "Howdy auth helper reached output limit");
				return PrepareIoResult::kFailed;
			}
			output->append(buffer.data(), static_cast<std::size_t>(size));
			return PrepareIoResult::kPending;
		}
		if (size == 0) {
			return PrepareIoResult::kComplete;
		}
		if (errno == EINTR || errno == EAGAIN) {
			return PrepareIoResult::kPending;
		}
		log_auth_helper_read_error(errno);
		return PrepareIoResult::kFailed;
	}

	auto receive_prepare_lease_once(int socket_fd, int *lease_fd) -> PrepareIoResult {
		const auto result = receive_lease_descriptor_once(socket_fd, lease_fd);
		if (result == LeaseReceiveResult::kReceived) {
			return PrepareIoResult::kComplete;
		}
		if (result == LeaseReceiveResult::kRetry) {
			return PrepareIoResult::kPending;
		}
		syslog(LOG_ERR, "Howdy auth helper returned malformed lease descriptor");
		return PrepareIoResult::kFailed;
	}

	auto poll_descriptor_failed(const pollfd &descriptor) -> bool {
		return descriptor.fd >= 0 && (descriptor.revents & (POLLERR | POLLNVAL)) != 0;
	}

	auto collect_prepare_response(const AuthHelperSpawnOperations &operations,
	                              PreparedHelperSpawn *spawn, HelperDeadline deadline,
	                              std::string *output, int *lease_fd) -> bool {
		output->clear();
		*lease_fd       = -1;
		bool output_eof = false;

		while (!output_eof || *lease_fd < 0) {
			std::array<pollfd, 2> descriptors = {
			    pollfd{
			        .fd = output_eof ? -1 : spawn->output_pipe[0], .events = POLLIN, .revents = 0},
			    pollfd{.fd      = *lease_fd >= 0 ? -1 : spawn->lease_socket[0],
			           .events  = POLLIN,
			           .revents = 0},
			};
			const int poll_result =
			    poll(descriptors.data(), descriptors.size(), deadline_poll_timeout(deadline));
			if (poll_result < 0 && errno == EINTR) {
				continue;
			}
			if (poll_result <= 0) {
				log_prepare_timeout(operations);
				return false;
			}

			if (poll_descriptor_failed(descriptors[0]) || poll_descriptor_failed(descriptors[1])) {
				return false;
			}
			if (descriptors[0].fd >= 0 && (descriptors[0].revents & (POLLIN | POLLHUP)) != 0) {
				const auto result = read_prepare_output_once(spawn->output_pipe[0], output);
				if (result == PrepareIoResult::kFailed) {
					return false;
				}
				output_eof = result == PrepareIoResult::kComplete;
			}

			if (descriptors[1].fd >= 0 && (descriptors[1].revents & (POLLIN | POLLHUP)) != 0 &&
			    receive_prepare_lease_once(spawn->lease_socket[0], lease_fd) ==
			        PrepareIoResult::kFailed) {
				return false;
			}
		}
		return true;
	}

	auto validate_prepare_helper(pid_t child_pid, HelperDeadline deadline,
	                             const std::string               &output,
	                             const AuthHelperSpawnOperations &operations) -> bool {
		int        status = 0;
		const auto result = wait_for_helper_process_until(child_pid, deadline, &status);
		if (result != HelperWaitResult::kExited) {
			if (result == HelperWaitResult::kTimedOut) {
				log_prepare_timeout(operations);
			} else {
				syslog(LOG_ERR, "Howdy auth helper failed while waiting");
			}
			return false;
		}
		if (!WIFEXITED(status) || WEXITSTATUS(status) != EXIT_SUCCESS) {
			syslog(LOG_ERR, "Howdy auth helper failed: %s", output.c_str());
			return false;
		}
		return true;
	}

	auto assign_prepared_paths(const std::string                &output,
	                           howdy::pam::PreparedRuntimeFiles *prepared) -> bool {
		const auto auth_output = parse_auth_helper_output(output);
		if (!auth_output.valid) {
			syslog(LOG_ERR, "Howdy auth helper returned malformed output: %s", output.c_str());
			return false;
		}
		prepared->config_path     = auth_output.config_path;
		prepared->user_models_dir = auth_output.user_models_dir;
		prepared->root_dir        = std::filesystem::path(prepared->config_path).parent_path();
		return true;
	}

	auto prepare_runtime_auth_files_until(std::string_view                  username,
	                                      howdy::pam::PreparedRuntimeFiles *prepared,
	                                      const AuthHelperSpawnOperations  &operations,
	                                      HelperDeadline                    deadline) -> bool {
		PreparedHelperSpawn spawn;
		if (!setup_helper_spawn(operations, &spawn)) {
			return false;
		}
		pid_t child_pid = -1;
		if (!spawn_prepare_helper(username, operations, &spawn, &child_pid)) {
			return false;
		}
		std::string helper_output;
		int         lease_fd = -1;
		const bool  collected =
		    collect_prepare_response(operations, &spawn, deadline, &helper_output, &lease_fd);
		close_owned_pipe_fd(operations, spawn.output_pipe[0]);
		if (!collected) {
			close_owned_pipe_fd(operations, spawn.lease_socket[0]);
			close_owned_pipe_fd(operations, lease_fd);
			(void)terminate_and_reap_helper_process(child_pid);
			return false;
		}
		if (!validate_prepare_helper(child_pid, deadline, helper_output, operations)) {
			close_owned_pipe_fd(operations, spawn.lease_socket[0]);
			close_owned_pipe_fd(operations, lease_fd);
			return false;
		}
		if (!lease_socket_has_clean_eof(spawn.lease_socket[0])) {
			syslog(LOG_ERR, "Howdy auth helper returned extra lease data");
			close_owned_pipe_fd(operations, spawn.lease_socket[0]);
			close_owned_pipe_fd(operations, lease_fd);
			return false;
		}
		close_owned_pipe_fd(operations, spawn.lease_socket[0]);
		if (!assign_prepared_paths(helper_output, prepared) ||
		    !validate_lease_descriptor_impl(lease_fd, prepared->root_dir, 0)) {
			syslog(LOG_ERR, "Howdy auth helper lease validation failed");
			close_owned_pipe_fd(operations, lease_fd);
			return false;
		}
		prepared->lease_fd = lease_fd;
		return true;
	}

}  // namespace

namespace howdy::pam::auth_helper_process {

	auto production_operations() -> Operations {
		return production_auth_helper_spawn_operations();
	}

	auto output_limit() -> std::size_t {
		return kAuthHelperOutputLimit;
	}

	auto receive_lease_descriptor(int socket_fd, int *lease_fd,
	                              std::chrono::steady_clock::time_point deadline) -> bool {
		return ::receive_lease_descriptor_until(socket_fd, lease_fd, deadline);
	}

	auto validate_lease_descriptor(int lease_fd, const std::filesystem::path &root_dir,
	                               uid_t owner_uid) -> bool {
		return ::validate_lease_descriptor_impl(lease_fd, root_dir, owner_uid);
	}

	auto prepare_runtime_auth_files(std::string_view username, PreparedRuntimeFiles *prepared,
	                                const Operations &operations) -> bool {
		return prepare_runtime_auth_files(username, prepared, operations,
		                                  std::chrono::steady_clock::now() + kAuthHelperTimeout);
	}

	auto prepare_runtime_auth_files(std::string_view username, PreparedRuntimeFiles *prepared,
	                                const Operations                     &operations,
	                                std::chrono::steady_clock::time_point deadline) -> bool {
		return ::prepare_runtime_auth_files_until(username, prepared, operations, deadline);
	}

	auto read_output(Process process, std::string *output, const Operations &operations,
	                 std::chrono::steady_clock::time_point deadline) -> bool {
		const auto read_result =
		    ::read_auth_helper_output_until(process.output_fd, output, operations, deadline);
		if (read_result != HelperReadResult::kComplete) {
			if (read_result == HelperReadResult::kTimedOut) {
				log_prepare_timeout(operations);
			}
			(void)terminate_and_reap_helper_process(process.child_pid);
			return false;
		}

		int        status = 0;
		const auto wait_result =
		    wait_for_helper_process_until(process.child_pid, deadline, &status);
		if (wait_result != HelperWaitResult::kExited) {
			output->clear();
			if (wait_result == HelperWaitResult::kTimedOut) {
				log_prepare_timeout(operations);
			}
			return false;
		}
		if (!WIFEXITED(status) || WEXITSTATUS(status) != EXIT_SUCCESS ||
		    !::parse_auth_helper_output(*output).valid) {
			output->clear();
			return false;
		}
		return true;
	}

	auto parse_output(const std::string &output) -> Output {
		return ::parse_auth_helper_output(output);
	}

	auto wait_for_helper(pid_t child_pid) -> int {
		return ::wait_for_helper_process(child_pid);
	}

	auto prepare_runtime_auth_files(std::string_view username, PreparedRuntimeFiles *prepared)
	    -> bool {
		auto operations         = production_operations();
		operations.read_bounded = nullptr;
		return ::prepare_runtime_auth_files_until(
		    username, prepared, operations, std::chrono::steady_clock::now() + kAuthHelperTimeout);
	}

}  // namespace howdy::pam::auth_helper_process
