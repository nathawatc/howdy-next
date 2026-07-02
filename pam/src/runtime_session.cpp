#include "runtime_session.hpp"

#include "common/auth_helper_protocol.hpp"
#include "common/compare_exit.hpp"
#include "common/fd_io.hpp"
#ifdef HOWDY_PAM_TESTING
#	include "auth_flow_testing.hpp"
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

	auto prepare_runtime_auth_files(std::string_view                  username,
	                                howdy::pam::PreparedRuntimeFiles *prepared) -> bool {
		std::array<int, 2> output_pipe = {-1, -1};
		if (pipe2(output_pipe.data(), O_CLOEXEC) != 0) {
			syslog(LOG_ERR, "Failed to create auth helper pipe: %s (%d)", strerror(errno), errno);
			return false;
		}

		posix_spawn_file_actions_t actions{};
		posix_spawn_file_actions_init(&actions);
		posix_spawn_file_actions_adddup2(&actions, output_pipe[1], STDOUT_FILENO);
		posix_spawn_file_actions_adddup2(&actions, output_pipe[1], STDERR_FILENO);
		posix_spawn_file_actions_addclose(&actions, output_pipe[0]);
		posix_spawn_file_actions_addclose(&actions, output_pipe[1]);

		std::string           username_string(username);
		std::array<char *, 4> args      = {const_cast<char *>(kAuthHelperPath),
		                                   const_cast<char *>("prepare"), username_string.data(),
		                                   nullptr};
		std::array<char *, 1> env       = {nullptr};
		pid_t                 child_pid = -1;
		const int             spawn_result =
		    posix_spawn(&child_pid, kAuthHelperPath, &actions, nullptr, args.data(), env.data());
		posix_spawn_file_actions_destroy(&actions);
		close(output_pipe[1]);

		if (spawn_result != 0) {
			close(output_pipe[0]);
			syslog(LOG_ERR, "Can't spawn the howdy auth helper: %s (%d)", strerror(spawn_result),
			       spawn_result);
			return false;
		}

		std::string helper_output;
		const bool  helper_ok = read_auth_helper_output(child_pid, output_pipe[0], &helper_output);
		close(output_pipe[0]);
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
