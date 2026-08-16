#ifndef _GNU_SOURCE
#	define _GNU_SOURCE
#endif

#include "runtime/auth_helper_process.hpp"

#include "protocol/auth_helper_protocol.hpp"
#include "protocol/compare_exit.hpp"
#include "runtime/runtime_session.hpp"

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

#include <sys/wait.h>

namespace {

	using howdy::native::CompareExit;

	constexpr std::size_t kAuthHelperOutputLimit  = 9216;
	constexpr auto        kAuthHelperTimeout      = std::chrono::seconds(10);
	constexpr auto        kHelperWaitPollInterval = std::chrono::milliseconds(10);

	using AuthHelperOutput          = howdy::pam::auth_helper_process::Output;
	using AuthHelperSpawnOperations = howdy::pam::auth_helper_process::Operations;
	using AuthHelperSpawnRequest    = howdy::pam::auth_helper_process::SpawnRequest;

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
		if (kill(child_pid, SIGTERM) != 0 && errno != ESRCH) {
			syslog(LOG_WARNING, "Failed to terminate auth helper process: %s (%d)", strerror(errno),
			       errno);
		}

		for (int attempts = 0; attempts < 50; ++attempts) {
			int         status      = 0;
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
			syslog(LOG_ERR, "Failed to read auth helper output: %s (%d)", strerror(read_error),
			       read_error);
			return HelperPollResult::kError;
		}
		if (poll_result == 0) {
			return HelperPollResult::kTimedOut;
		}
		if ((descriptor.revents & (POLLNVAL | POLLERR)) != 0) {
			const int read_error = (descriptor.revents & POLLNVAL) != 0 ? EBADF : EIO;
			syslog(LOG_ERR, "Failed to read auth helper output: %s (%d)", strerror(read_error),
			       read_error);
			return HelperPollResult::kError;
		}
		return (descriptor.revents & (POLLIN | POLLHUP)) == 0 ? HelperPollResult::kRetry
		                                                      : HelperPollResult::kReady;
	}

	auto read_auth_helper_output_from_fd(int output_fd, std::string &output,
	                                     HelperDeadline deadline) -> HelperReadResult {
		if (output_fd < 0) {
			constexpr int read_error = EBADF;
			syslog(LOG_ERR, "Failed to read auth helper output: %s (%d)", strerror(read_error),
			       read_error);
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
				syslog(LOG_ERR, "Failed to read auth helper output: %s (%d)", strerror(read_error),
				       read_error);
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
				syslog(LOG_ERR, "Failed to read auth helper output: %s (%d)",
				       strerror(helper_output.error_number), helper_output.error_number);
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

	void log_cleanup_timeout(const AuthHelperSpawnOperations &operations) {
		constexpr std::string_view message = "Howdy auth helper cleanup timed out";
		syslog(LOG_WARNING, "%.*s", static_cast<int>(message.size()), message.data());
		capture_auth_helper_log(operations, message);
	}

	void write_helper_cleanup_failure_log(const AuthHelperSpawnOperations &operations) {
		constexpr std::string_view message = "Howdy auth helper cleanup failed";
		syslog(LOG_WARNING, "%.*s", static_cast<int>(message.size()), message.data());
		capture_auth_helper_log(operations, message);
	}

	auto production_pipe2(void *context, int *pipe_fds, int flags) -> int {
		(void)context;
		return pipe2(pipe_fds, flags);
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

	auto normalize_pipe_fds(const AuthHelperSpawnOperations &operations,
	                        std::array<int, 2>              &output_pipe) -> bool {
		for (int &pipe_fd : output_pipe) {
			if (pipe_fd > STDERR_FILENO) {
				continue;
			}
			const int normalized_fd =
			    operations.duplicate_fd(operations.context, pipe_fd, STDERR_FILENO + 1);
			if (normalized_fd < 0) {
				write_helper_spawn_error_log(operations, "fcntl(F_DUPFD_CLOEXEC)", errno);
				close_owned_pipe_fd(operations, output_pipe[0]);
				close_owned_pipe_fd(operations, output_pipe[1]);
				return false;
			}
			close_owned_pipe_fd(operations, pipe_fd);
			pipe_fd = normalized_fd;
		}
		return true;
	}

	struct PreparedHelperSpawn {
		std::array<int, 2>         output_pipe = {-1, -1};
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
		return false;
	}

	auto setup_helper_spawn(const AuthHelperSpawnOperations &operations, PreparedHelperSpawn *spawn)
	    -> bool {
		if (operations.pipe2(operations.context, spawn->output_pipe.data(), O_CLOEXEC) != 0) {
			syslog(LOG_ERR, "Failed to create auth helper pipe: %s (%d)", strerror(errno), errno);
			return false;
		}
		if (!normalize_pipe_fds(operations, spawn->output_pipe)) {
			return false;
		}
		const int init_result = operations.actions_init(operations.context, &spawn->actions);
		if (init_result != 0) {
			write_helper_spawn_error_log(operations, "posix_spawn_file_actions_init", init_result);
			close_owned_pipe_fd(operations, spawn->output_pipe[0]);
			close_owned_pipe_fd(operations, spawn->output_pipe[1]);
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
		result =
		    operations.actions_addclosefrom(operations.context, &spawn->actions, STDERR_FILENO + 1);
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
		if (result != 0) {
			close_owned_pipe_fd(operations, spawn->output_pipe[0]);
			return false;
		}
		return true;
	}

	auto collect_prepare_output(const AuthHelperSpawnOperations &operations,
	                            PreparedHelperSpawn *spawn, pid_t child_pid,
	                            HelperDeadline deadline, std::string *output) -> bool {
		const auto result =
		    read_auth_helper_output_until(spawn->output_pipe[0], output, operations, deadline);
		close_owned_pipe_fd(operations, spawn->output_pipe[0]);
		if (result == HelperReadResult::kComplete) {
			return true;
		}
		if (result == HelperReadResult::kTimedOut) {
			log_prepare_timeout(operations);
		} else if (result == HelperReadResult::kOutputLimit) {
			syslog(LOG_ERR, "Howdy auth helper reached output limit");
		}
		(void)terminate_and_reap_helper_process(child_pid);
		return false;
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
		if (!collect_prepare_output(operations, &spawn, child_pid, deadline, &helper_output)) {
			return false;
		}
		if (!validate_prepare_helper(child_pid, deadline, helper_output, operations)) {
			return false;
		}
		return assign_prepared_paths(helper_output, prepared);
	}

	auto cleanup_runtime_auth_files_until(const std::filesystem::path     &root_dir,
	                                      const AuthHelperSpawnOperations &operations,
	                                      HelperDeadline                   deadline) -> void {
		std::string                root_dir_string = root_dir.string();
		std::array<char *, 4>      args = {const_cast<char *>(kAuthHelperPath),
		                                   const_cast<char *>("cleanup"),
		                                   const_cast<char *>(root_dir_string.c_str()), nullptr};
		std::array<char *, 1>      env  = {nullptr};
		posix_spawn_file_actions_t file_actions{};
		const int init_result = operations.actions_init(operations.context, &file_actions);
		if (init_result != 0) {
			write_helper_spawn_error_log(operations, "posix_spawn_file_actions_init", init_result);
			return;
		}

		const int closefrom_result =
		    operations.actions_addclosefrom(operations.context, &file_actions, STDERR_FILENO + 1);
		if (closefrom_result != 0) {
			write_helper_spawn_error_log(operations, "posix_spawn_file_actions_addclosefrom",
			                             closefrom_result);
			destroy_spawn_actions(operations, &file_actions);
			return;
		}

		pid_t     child_pid    = -1;
		const int spawn_result = operations.spawn({.context   = operations.context,
		                                           .child_pid = &child_pid,
		                                           .path      = kAuthHelperPath,
		                                           .actions   = &file_actions,
		                                           .argv      = args.data(),
		                                           .envp      = env.data()});
		if (spawn_result != 0) {
			syslog(LOG_WARNING, "Can't spawn the howdy auth helper cleanup: %s (%d)",
			       strerror(spawn_result), spawn_result);
		}
		destroy_spawn_actions(operations, &file_actions);
		if (spawn_result != 0) {
			return;
		}

		int        status      = 0;
		const auto wait_result = wait_for_helper_process_until(child_pid, deadline, &status);
		if (wait_result == HelperWaitResult::kTimedOut) {
			log_cleanup_timeout(operations);
			return;
		}
		if (wait_result != HelperWaitResult::kExited || !WIFEXITED(status) ||
		    WEXITSTATUS(status) != EXIT_SUCCESS) {
			write_helper_cleanup_failure_log(operations);
		}
	}

	auto cleanup_runtime_auth_files(const std::filesystem::path &root_dir) -> void {
		auto operations         = production_auth_helper_spawn_operations();
		operations.read_bounded = nullptr;
		cleanup_runtime_auth_files_until(root_dir, operations,
		                                 std::chrono::steady_clock::now() + kAuthHelperTimeout);
	}
}  // namespace

namespace howdy::pam::auth_helper_process {

	auto production_operations() -> Operations {
		return production_auth_helper_spawn_operations();
	}

	auto output_limit() -> std::size_t {
		return kAuthHelperOutputLimit;
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

	auto cleanup_runtime_auth_files(const std::filesystem::path          &root_dir,
	                                const Operations                     &operations,
	                                std::chrono::steady_clock::time_point deadline) -> void {
		::cleanup_runtime_auth_files_until(root_dir, operations, deadline);
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

	auto wait_for_cleanup_helper(pid_t child_pid, const Operations &operations,
	                             std::chrono::steady_clock::time_point deadline) -> bool {
		int        status      = 0;
		const auto wait_result = wait_for_helper_process_until(child_pid, deadline, &status);
		if (wait_result == HelperWaitResult::kTimedOut) {
			log_cleanup_timeout(operations);
			return false;
		}
		if (wait_result != HelperWaitResult::kExited || !WIFEXITED(status) ||
		    WEXITSTATUS(status) != EXIT_SUCCESS) {
			write_helper_cleanup_failure_log(operations);
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

	auto cleanup_runtime_auth_files(const std::filesystem::path &root_dir) -> void {
		::cleanup_runtime_auth_files(root_dir);
	}

}  // namespace howdy::pam::auth_helper_process
