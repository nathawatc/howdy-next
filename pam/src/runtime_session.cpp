#include "runtime_session.hpp"

#include "common/auth_helper_protocol.hpp"
#include "common/compare_exit.hpp"
#include "common/fd_io.hpp"
#ifdef HOWDY_PAM_TESTING
#	include "auth_flow_testing.hpp"
#	include "runtime_session_testing.hpp"
#endif

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <paths.hpp>
#include <spawn.h>
#include <string>
#include <string_view>
#include <syslog.h>
#include <unistd.h>
#include <utility>

#include <sys/wait.h>

namespace {

	using howdy::native::CompareExit;

	constexpr std::size_t kAuthHelperOutputLimit = 9216;

#ifdef HOWDY_PAM_TESTING
	using AuthHelperOutputReader = howdy::native::BoundedReadResult (*)(int, std::size_t);
	AuthHelperOutputReader g_auth_helper_output_reader = nullptr;
#endif

	struct AuthHelperOutput {
		std::string config_path;
		std::string user_models_dir;
		bool        valid = false;
	};

	auto make_wait_exit_status(CompareExit exit_code) -> int {
		return static_cast<int>(exit_code) << 8;
	}

	auto read_bounded_auth_helper_output(int output_fd) -> howdy::native::BoundedReadResult {
#ifdef HOWDY_PAM_TESTING
		if (g_auth_helper_output_reader != nullptr) {
			return g_auth_helper_output_reader(output_fd, kAuthHelperOutputLimit);
		}
#endif
		return howdy::native::read_fd_to_string_bounded(output_fd, kAuthHelperOutputLimit);
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

	auto read_auth_helper_output(pid_t child_pid, int output_fd, std::string *output) -> bool {
		if (output == nullptr) {
			terminate_and_reap_helper_process(child_pid);
			return false;
		}

		const auto helper_output = read_bounded_auth_helper_output(output_fd);

		if (helper_output.read_error) {
			output->clear();
			syslog(LOG_ERR, "Failed to read auth helper output: %s (%d)",
			       strerror(helper_output.error_number), helper_output.error_number);
			terminate_and_reap_helper_process(child_pid);
			return false;
		}

		if (helper_output.hit_limit) {
			output->clear();
			syslog(LOG_ERR, "Howdy auth helper reached output limit");
			terminate_and_reap_helper_process(child_pid);
			return false;
		}

		*output          = helper_output.output;
		const int status = wait_for_helper_process(child_pid);
		if (!WIFEXITED(status) || WEXITSTATUS(status) != EXIT_SUCCESS) {
			const auto failed_output = *output;
			output->clear();
			syslog(LOG_ERR, "Howdy auth helper failed: %s", failed_output.c_str());
			return false;
		}

		const auto auth_output = parse_auth_helper_output(*output);
		if (!auth_output.valid) {
			const auto malformed_output = *output;
			output->clear();
			syslog(LOG_ERR, "Howdy auth helper returned malformed output: %s",
			       malformed_output.c_str());
			return false;
		}
		return true;
	}

	struct AuthHelperSpawnOperations {
		void *context = nullptr;

		int (*pipe2_fn)(void *, int *pipe_fds, int flags)                        = nullptr;
		int (*duplicate_fd_fn)(void *, int fd, int minimum_fd)                   = nullptr;
		int (*actions_init_fn)(void *, posix_spawn_file_actions_t *)             = nullptr;
		int (*actions_adddup2_fn)(void *, posix_spawn_file_actions_t *, int source_fd,
		                          int target_fd)                                 = nullptr;
		int (*actions_addclose_fn)(void *, posix_spawn_file_actions_t *, int fd) = nullptr;
		int (*actions_destroy_fn)(void *, posix_spawn_file_actions_t *)          = nullptr;
		int (*spawn_fn)(void *, pid_t *, const char *, const posix_spawn_file_actions_t *,
		                char *const *argv, char *const *envp)                    = nullptr;
		int (*close_fn)(void *, int fd)                                          = nullptr;
	};

#ifdef HOWDY_PAM_TESTING
	howdy::pam::testing::AuthHelperSpawnLogFn g_auth_helper_spawn_log_fn = nullptr;
#endif

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

	auto production_actions_destroy(void *context, posix_spawn_file_actions_t *actions) -> int {
		(void)context;
		return posix_spawn_file_actions_destroy(actions);
	}

	auto production_spawn(void *context, pid_t *child_pid, const char *path,
	                      const posix_spawn_file_actions_t *actions, char *const *argv,
	                      char *const *envp) -> int {
		(void)context;
		return posix_spawn(child_pid, path, actions, nullptr, argv, envp);
	}

	auto production_close(void *context, int fd) -> int {
		(void)context;
		return close(fd);
	}

	auto production_auth_helper_spawn_operations() -> AuthHelperSpawnOperations {
		return {
		    .pipe2_fn            = production_pipe2,
		    .duplicate_fd_fn     = production_duplicate_fd,
		    .actions_init_fn     = production_actions_init,
		    .actions_adddup2_fn  = production_actions_adddup2,
		    .actions_addclose_fn = production_actions_addclose,
		    .actions_destroy_fn  = production_actions_destroy,
		    .spawn_fn            = production_spawn,
		    .close_fn            = production_close,
		};
	}

	void log_auth_helper_spawn_error(const char *operation, int error_code) {
		syslog(LOG_ERR, "%s failed for auth helper: %s (%d)", operation, strerror(error_code),
		       error_code);
#ifdef HOWDY_PAM_TESTING
		if (g_auth_helper_spawn_log_fn != nullptr) {
			const std::string message = std::string(operation) +
			                            " failed for auth helper: " + strerror(error_code) + " (" +
			                            std::to_string(error_code) + ")";
			g_auth_helper_spawn_log_fn(message);
		}
#endif
	}

	void close_owned_pipe_fd(const AuthHelperSpawnOperations &operations, int &fd) {
		if (fd < 0) {
			return;
		}
		(void)operations.close_fn(operations.context, fd);
		fd = -1;
	}

	auto prepare_runtime_auth_files(std::string_view                  username,
	                                howdy::pam::PreparedRuntimeFiles *prepared,
	                                const AuthHelperSpawnOperations  &operations) -> bool {
		std::array<int, 2> output_pipe = {-1, -1};
		if (operations.pipe2_fn(operations.context, output_pipe.data(), O_CLOEXEC) != 0) {
			syslog(LOG_ERR, "Failed to create auth helper pipe: %s (%d)", strerror(errno), errno);
			return false;
		}

		for (int &pipe_fd : output_pipe) {
			if (pipe_fd > STDERR_FILENO) {
				continue;
			}
			const int normalized_fd =
			    operations.duplicate_fd_fn(operations.context, pipe_fd, STDERR_FILENO + 1);
			if (normalized_fd < 0) {
				const int duplicate_error = errno;
				log_auth_helper_spawn_error("fcntl(F_DUPFD_CLOEXEC)", duplicate_error);
				close_owned_pipe_fd(operations, output_pipe[0]);
				close_owned_pipe_fd(operations, output_pipe[1]);
				return false;
			}
			close_owned_pipe_fd(operations, pipe_fd);
			pipe_fd = normalized_fd;
		}

		posix_spawn_file_actions_t actions{};
		const int actions_init_result = operations.actions_init_fn(operations.context, &actions);
		if (actions_init_result != 0) {
			log_auth_helper_spawn_error("posix_spawn_file_actions_init", actions_init_result);
			close_owned_pipe_fd(operations, output_pipe[0]);
			close_owned_pipe_fd(operations, output_pipe[1]);
			return false;
		}

		auto destroy_actions = [&operations, &actions]() -> int {
			const int destroy_result = operations.actions_destroy_fn(operations.context, &actions);
			if (destroy_result != 0) {
				log_auth_helper_spawn_error("posix_spawn_file_actions_destroy", destroy_result);
			}
			return destroy_result;
		};

		auto fail_setup = [&operations, &output_pipe, &destroy_actions](const char *operation,
		                                                                int error_code) -> bool {
			log_auth_helper_spawn_error(operation, error_code);
			(void)destroy_actions();
			close_owned_pipe_fd(operations, output_pipe[0]);
			close_owned_pipe_fd(operations, output_pipe[1]);
			return false;
		};

		int result = operations.actions_addclose_fn(operations.context, &actions, output_pipe[0]);
		if (result != 0) {
			return fail_setup("posix_spawn_file_actions_addclose(pipe read end)", result);
		}

		result = operations.actions_adddup2_fn(operations.context, &actions, output_pipe[1],
		                                       STDOUT_FILENO);
		if (result != 0) {
			return fail_setup("posix_spawn_file_actions_adddup2(STDOUT_FILENO)", result);
		}

		result = operations.actions_adddup2_fn(operations.context, &actions, output_pipe[1],
		                                       STDERR_FILENO);
		if (result != 0) {
			return fail_setup("posix_spawn_file_actions_adddup2(STDERR_FILENO)", result);
		}

		if (output_pipe[1] != STDOUT_FILENO && output_pipe[1] != STDERR_FILENO) {
			result = operations.actions_addclose_fn(operations.context, &actions, output_pipe[1]);
			if (result != 0) {
				return fail_setup("posix_spawn_file_actions_addclose(pipe write end)", result);
			}
		}

		std::string           username_string(username);
		std::array<char *, 4> args         = {const_cast<char *>(kAuthHelperPath),
		                                      const_cast<char *>("prepare"), username_string.data(),
		                                      nullptr};
		std::array<char *, 1> env          = {nullptr};
		pid_t                 child_pid    = -1;
		const int             spawn_result = operations.spawn_fn(
		    operations.context, &child_pid, kAuthHelperPath, &actions, args.data(), env.data());
		if (spawn_result != 0) {
			log_auth_helper_spawn_error("posix_spawn", spawn_result);
		}
		(void)destroy_actions();
		close_owned_pipe_fd(operations, output_pipe[1]);

		if (spawn_result != 0) {
			close_owned_pipe_fd(operations, output_pipe[0]);
			return false;
		}

		std::string helper_output;
		const bool  helper_ok = read_auth_helper_output(child_pid, output_pipe[0], &helper_output);
		close_owned_pipe_fd(operations, output_pipe[0]);
		if (!helper_ok) {
			return false;
		}

		const auto auth_output = parse_auth_helper_output(helper_output);
		if (!auth_output.valid) {
			syslog(LOG_ERR, "Howdy auth helper returned malformed output: %s",
			       helper_output.c_str());
			return false;
		}
		prepared->config_path     = auth_output.config_path;
		prepared->user_models_dir = auth_output.user_models_dir;
		prepared->root_dir        = std::filesystem::path(prepared->config_path).parent_path();
		return true;
	}

	auto prepare_runtime_auth_files(std::string_view                  username,
	                                howdy::pam::PreparedRuntimeFiles *prepared) -> bool {
		return prepare_runtime_auth_files(username, prepared,
		                                  production_auth_helper_spawn_operations());
	}

	auto cleanup_runtime_auth_files(const std::filesystem::path &root_dir) -> void {
		std::string           root_dir_string = root_dir.string();
		std::array<char *, 4> args      = {const_cast<char *>(kAuthHelperPath),
		                                   const_cast<char *>("cleanup"),
		                                   const_cast<char *>(root_dir_string.c_str()), nullptr};
		std::array<char *, 1> env       = {nullptr};
		pid_t                 child_pid = -1;
		const int             spawn_result =
		    posix_spawn(&child_pid, kAuthHelperPath, nullptr, nullptr, args.data(), env.data());
		if (spawn_result != 0) {
			syslog(LOG_WARNING, "Can't spawn the howdy auth helper cleanup: %s (%d)",
			       strerror(spawn_result), spawn_result);
			return;
		}

		const int status = wait_for_helper_process(child_pid);
		if (!WIFEXITED(status) || WEXITSTATUS(status) != EXIT_SUCCESS) {
			syslog(LOG_WARNING, "Howdy auth helper cleanup failed");
		}
	}

	auto prepare_runtime_files_dependency(void *context, std::string_view username,
	                                      howdy::pam::PreparedRuntimeFiles *prepared) -> bool {
		(void)context;
		return prepare_runtime_auth_files(username, prepared);
	}

	auto cleanup_runtime_files_dependency(void *context, const std::filesystem::path &root_dir)
	    -> void {
		(void)context;
		cleanup_runtime_auth_files(root_dir);
	}

	auto load_runtime_config_dependency(void *context, const std::filesystem::path &config_path)
	    -> howdy::native::RuntimeConfigLoadResult {
		(void)context;
		return howdy::native::load_runtime_config(config_path, static_cast<uid_t>(0));
	}

	auto effective_uid_dependency(void *context) -> uid_t {
		(void)context;
		return geteuid();
	}

}  // namespace

namespace howdy::pam {
	namespace {

		auto invoke_cleanup(const RuntimeSessionDependencies &dependencies,
		                    const std::filesystem::path      &runtime_root) noexcept -> void {
			try {
				dependencies.cleanup_runtime(dependencies.context, runtime_root);
			} catch (const std::exception &error) {
				syslog(LOG_WARNING, "Howdy auth helper cleanup failed: %s", error.what());
			} catch (...) {
				syslog(LOG_WARNING, "Howdy auth helper cleanup failed with non-standard exception");
			}
		}

	}  // namespace

	RuntimeSession::RuntimeSession(std::string                configured_config_path,
	                               std::string                configured_user_models_dir,
	                               RuntimeSessionDependencies dependencies)
	    : config_path_(std::move(configured_config_path))
	    , user_models_dir_(std::move(configured_user_models_dir))
	    , dependencies_(dependencies) {}

	RuntimeSession::~RuntimeSession() {
		if (!cleanup_active_ || runtime_root_.empty()) {
			return;
		}

		cleanup_active_ = false;
		invoke_cleanup(dependencies_, runtime_root_);
	}

	auto RuntimeSession::load_for_user(std::string_view username) -> RuntimeSessionLoadResult {
		if (load_started_) {
			return {
			    .status = RuntimeSessionLoadStatus::kAlreadyLoaded,
			};
		}
		load_started_ = true;

		if (dependencies_.prepare_runtime == nullptr || dependencies_.cleanup_runtime == nullptr ||
		    dependencies_.load_runtime_config == nullptr ||
		    dependencies_.effective_uid == nullptr) {
			return {};
		}

		auto config_result = dependencies_.load_runtime_config(dependencies_.context, config_path_);
		const bool needs_staging =
		    config_result.status == howdy::native::RuntimeConfigLoadStatus::kPathError &&
		    config_result.error_code == EACCES &&
		    dependencies_.effective_uid(dependencies_.context) != 0;
		if (!needs_staging) {
			const bool config_ok =
			    config_result.status == howdy::native::RuntimeConfigLoadStatus::kOk &&
			    config_result.config.has_value();
			return RuntimeSessionLoadResult{
			    .status        = config_ok ? RuntimeSessionLoadStatus::kOk
			                               : RuntimeSessionLoadStatus::kConfigLoadFailed,
			    .config_result = std::move(config_result),
			};
		}

		PreparedRuntimeFiles prepared;
		if (!dependencies_.prepare_runtime(dependencies_.context, username, &prepared)) {
			return RuntimeSessionLoadResult{
			    .status        = RuntimeSessionLoadStatus::kPrepareFailed,
			    .config_result = std::move(config_result),
			};
		}
		if (prepared.config_path.empty() || prepared.user_models_dir.empty() ||
		    prepared.root_dir.empty()) {
			if (!prepared.root_dir.empty()) {
				invoke_cleanup(dependencies_, prepared.root_dir);
			}
			return RuntimeSessionLoadResult{
			    .status        = RuntimeSessionLoadStatus::kPrepareFailed,
			    .config_result = std::move(config_result),
			};
		}

		config_path_     = std::move(prepared.config_path);
		user_models_dir_ = std::move(prepared.user_models_dir);
		runtime_root_    = std::move(prepared.root_dir);
		cleanup_active_  = true;

		config_result = dependencies_.load_runtime_config(dependencies_.context, config_path_);
		const bool config_ok =
		    config_result.status == howdy::native::RuntimeConfigLoadStatus::kOk &&
		    config_result.config.has_value();
		return RuntimeSessionLoadResult{
		    .status        = config_ok ? RuntimeSessionLoadStatus::kOk
		                               : RuntimeSessionLoadStatus::kConfigLoadFailed,
		    .config_result = std::move(config_result),
		};
	}

	auto RuntimeSession::config_path() const -> const std::string & {
		return config_path_;
	}

	auto RuntimeSession::user_models_dir() const -> const std::string & {
		return user_models_dir_;
	}

	auto RuntimeSession::staged() const -> bool {
		return cleanup_active_;
	}

	auto production_runtime_session_dependencies() -> RuntimeSessionDependencies {
		return RuntimeSessionDependencies{
		    .prepare_runtime     = prepare_runtime_files_dependency,
		    .cleanup_runtime     = cleanup_runtime_files_dependency,
		    .load_runtime_config = load_runtime_config_dependency,
		    .effective_uid       = effective_uid_dependency,
		};
	}

}  // namespace howdy::pam

#ifdef HOWDY_PAM_TESTING
namespace howdy::pam::testing {

	auto auth_helper_output_limit() -> std::size_t {
		return kAuthHelperOutputLimit;
	}

	auto prepare_runtime_auth_files(std::string_view username, PreparedRuntimeFiles *prepared,
	                                const AuthHelperSpawnOperations &operations) -> bool {
		return ::prepare_runtime_auth_files(
		    username, prepared,
		    ::AuthHelperSpawnOperations{
		        .context             = operations.context,
		        .pipe2_fn            = operations.pipe2_fn,
		        .duplicate_fd_fn     = operations.duplicate_fd_fn,
		        .actions_init_fn     = operations.actions_init_fn,
		        .actions_adddup2_fn  = operations.actions_adddup2_fn,
		        .actions_addclose_fn = operations.actions_addclose_fn,
		        .actions_destroy_fn  = operations.actions_destroy_fn,
		        .spawn_fn            = operations.spawn_fn,
		        .close_fn            = operations.close_fn,
		    });
	}

	auto set_auth_helper_spawn_log_fn(AuthHelperSpawnLogFn logger) -> AuthHelperSpawnLogFn {
		const auto previous_logger = g_auth_helper_spawn_log_fn;
		g_auth_helper_spawn_log_fn = logger;
		return previous_logger;
	}

	auto set_auth_helper_output_reader(AuthHelperOutputReader reader) -> AuthHelperOutputReader {
		const auto previous_reader  = g_auth_helper_output_reader;
		g_auth_helper_output_reader = reader;
		return previous_reader;
	}

	auto read_fd_to_string(int fd) -> std::string {
		return howdy::native::read_fd_to_string_bounded(fd, kAuthHelperOutputLimit).output;
	}

	auto read_auth_helper_output(pid_t child_pid, int output_fd, std::string *output) -> bool {
		return ::read_auth_helper_output(child_pid, output_fd, output);
	}

	auto parse_auth_helper_output(const std::string &output) -> AuthHelperOutput {
		const auto parsed = ::parse_auth_helper_output(output);
		return AuthHelperOutput{.config_path     = parsed.config_path,
		                        .user_models_dir = parsed.user_models_dir,
		                        .valid           = parsed.valid};
	}

	auto wait_for_helper_process(pid_t child_pid) -> int {
		return ::wait_for_helper_process(child_pid);
	}

}  // namespace howdy::pam::testing
#endif
