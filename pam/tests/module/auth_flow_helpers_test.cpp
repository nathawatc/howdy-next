#include "config/runtime_config.hpp"
#include "module/auth_flow.hpp"
#include "module/main.hpp"
#include "protocol/auth_helper_protocol.hpp"
#include "protocol/compare_exit.hpp"
#include "runtime/auth_helper_process.hpp"
#include "runtime/compare_process.hpp"
#include "support/fd_io.hpp"
#include "test_support.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <clocale>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <libintl.h>
#include <limits>
#include <optional>
#include <string>
#include <tuple>
#include <unistd.h>
#include <utility>
#include <vector>

#include <security/pam_appl.h>
#include <security/pam_modules.h>

#include <sys/stat.h>
#include <sys/wait.h>

namespace {

	using howdy::test::expect;

	auto fake_partial_read_error(void                                              *context,
	                             [[maybe_unused]] howdy::native::BoundedReadRequest request)
	    -> howdy::native::BoundedReadResult {
		(void)context;
		howdy::native::BoundedReadResult result;
		result.output       = "CONFIG_PATH=/tmp/partial\n";
		result.read_error   = true;
		result.error_number = EIO;
		return result;
	}

	auto read_auth_helper_output(pid_t child_pid, int output_fd, std::string *output,
	                             howdy::pam::auth_helper_process::OutputReader reader = nullptr)
	    -> bool {
		auto operations         = howdy::pam::auth_helper_process::production_operations();
		operations.read_bounded = reader;
		return howdy::pam::auth_helper_process::read_output(
		    {.child_pid = child_pid, .output_fd = output_fd}, output, operations,
		    std::chrono::steady_clock::now() + std::chrono::seconds(10));
	}

	auto read_fd_to_string(int fd) -> std::string {
		return howdy::native::read_fd_to_string_bounded(
		           {.fd = fd, .max_bytes = howdy::pam::auth_helper_process::output_limit()})
		    .output;
	}

	class ScopedFd {
	public:
		ScopedFd() = default;

		explicit ScopedFd(int fd)
		    : fd_(fd) {}

		ScopedFd(const ScopedFd &)                     = delete;
		auto operator=(const ScopedFd &) -> ScopedFd & = delete;

		ScopedFd(ScopedFd &&other) noexcept
		    : fd_(other.release()) {}

		auto operator=(ScopedFd &&other) noexcept -> ScopedFd & {
			if (this != &other) {
				reset(other.release());
			}
			return *this;
		}

		~ScopedFd() {
			reset();
		}

		[[nodiscard]] auto get() const -> int {
			return fd_;
		}

		void reset(int fd = -1) {
			if (fd_ >= 0) {
				close(fd_);
			}
			fd_ = fd;
		}

		auto release() -> int {
			const int fd = fd_;
			fd_          = -1;
			return fd;
		}

	private:
		int fd_ = -1;
	};

	class ScopedPamHandle {
	public:
		ScopedPamHandle() = default;

		ScopedPamHandle(const ScopedPamHandle &)                     = delete;
		auto operator=(const ScopedPamHandle &) -> ScopedPamHandle & = delete;

		~ScopedPamHandle() {
			if (pamh_ != nullptr) {
				pam_end(pamh_, PAM_SUCCESS);
			}
		}

		auto start(const struct pam_conv *conversation) -> int {
			return pam_start("howdy-auth-flow-test", "test-user", conversation, &pamh_);
		}

		[[nodiscard]] auto get() const -> pam_handle_t * {
			return pamh_;
		}

	private:
		pam_handle_t *pamh_ = nullptr;
	};

	class ScopedEnv {
	public:
		explicit ScopedEnv(const char *name)
		    : name_(name) {
			const char *value = getenv(name_.c_str());
			if (value != nullptr) {
				original_ = value;
			}
		}

		ScopedEnv(const ScopedEnv &)                     = delete;
		auto operator=(const ScopedEnv &) -> ScopedEnv & = delete;

		~ScopedEnv() {
			if (original_.has_value()) {
				setenv(name_.c_str(), original_->c_str(), 1);
				return;
			}
			unsetenv(name_.c_str());
		}

	private:
		std::string                name_;
		std::optional<std::string> original_;
	};

	struct TemporaryFile {
		std::string path;
		ScopedFd    fd;
	};

	enum class ResponseMode : std::uint8_t {
		None,
		Empty,
		Secret,
	};

	struct ConversationState {
		int          result        = PAM_SUCCESS;
		int          calls         = 0;
		int          last_msg_type = 0;
		std::string  last_message;
		ResponseMode response_mode = ResponseMode::None;
	};

	auto test_conversation(int num_msg, const struct pam_message **messages,
	                       struct pam_response **response, void *appdata_ptr) -> int {
		auto *state = static_cast<ConversationState *>(appdata_ptr);
		if (state == nullptr || num_msg != 1 || messages == nullptr || messages[0] == nullptr ||
		    response == nullptr) {
			return PAM_CONV_ERR;
		}

		++state->calls;
		state->last_msg_type = messages[0]->msg_style;
		state->last_message  = messages[0]->msg == nullptr ? "" : messages[0]->msg;
		*response            = nullptr;

		if (state->response_mode != ResponseMode::None) {
			*response = static_cast<struct pam_response *>(calloc(1, sizeof(struct pam_response)));
			if (*response == nullptr) {
				return PAM_BUF_ERR;
			}
			if (state->response_mode == ResponseMode::Secret) {
				(*response)->resp = strdup("temporary-secret");
				if ((*response)->resp == nullptr) {
					free(*response);
					*response = nullptr;
					return PAM_BUF_ERR;
				}
			}
		}

		return state->result;
	}

	auto open_pipe(std::array<ScopedFd, 2> *fds) -> bool {
		std::array<int, 2> raw_fds{{-1, -1}};
		if (pipe(raw_fds.data()) != 0) {
			return false;
		}
		(*fds)[0].reset(raw_fds[0]);
		(*fds)[1].reset(raw_fds[1]);
		return true;
	}

	auto write_all(int fd, const std::string &data) -> bool {
		return howdy::native::write_all_to_fd(fd, data);
	}

	auto write_file(const std::filesystem::path &path, const std::string &content) -> bool {
		std::ofstream output(path);
		output << content;
		return output.good();
	}

	auto create_temp_file(const std::string &label) -> std::optional<TemporaryFile> {
		const std::string template_path = "/tmp/howdy-auth-flow-" + label + "-XXXXXX";
		std::vector<char> path_buffer(template_path.begin(), template_path.end());
		path_buffer.push_back('\0');

		ScopedFd fd(mkstemp(path_buffer.data()));
		if (fd.get() < 0) {
			return std::nullopt;
		}
		return TemporaryFile{.path = path_buffer.data(), .fd = std::move(fd)};
	}

	auto create_temp_directory(const std::string &template_path)
	    -> std::optional<std::filesystem::path> {
		std::vector<char> path_buffer(template_path.begin(), template_path.end());
		path_buffer.push_back('\0');

		char *created = mkdtemp(path_buffer.data());
		if (created == nullptr) {
			return std::nullopt;
		}
		return std::filesystem::path(created);
	}

	auto expect_fd_reading() -> bool {
		bool                    ok = true;
		std::array<ScopedFd, 2> empty_pipe;
		ok &= expect(open_pipe(&empty_pipe), "creates empty input pipe");
		empty_pipe[1].reset();
		ok &= expect(read_fd_to_string(empty_pipe[0].get()).empty(), "reads empty input");
		ok &= expect(read_fd_to_string(-1).empty(), "read failure returns collected empty output");

		std::array<ScopedFd, 2> small_pipe;
		ok &= expect(open_pipe(&small_pipe), "creates small input pipe");
		const std::string small_output = "small helper output\n";
		ok &= expect(write_all(small_pipe[1].get(), small_output), "writes small helper output");
		small_pipe[1].reset();
		ok &= expect(read_fd_to_string(small_pipe[0].get()) == small_output,
		             "reads complete small helper output");

		auto bounded_file = create_temp_file("output");
		ok &= expect(bounded_file.has_value(), "creates bounded input file");
		if (!bounded_file.has_value()) {
			return false;
		}
		unlink(bounded_file->path.c_str());
		const std::string limit_output(16384, 'x');
		ok &= expect(write_all(bounded_file->fd.get(), limit_output),
		             "writes oversized helper output");
		ok &= expect(lseek(bounded_file->fd.get(), 0, SEEK_SET) == 0,
		             "rewinds oversized helper output");
		const std::string bounded_output = read_fd_to_string(bounded_file->fd.get());
		ok &= expect(bounded_output == limit_output.substr(0, 9216),
		             "stops reading after bounded output threshold");

		return ok;
	}

	auto spawn_exiting_child(int exit_code) -> pid_t {
		const pid_t child_pid = fork();
		if (child_pid == 0) {
			_exit(exit_code);
		}
		return child_pid;
	}

	auto expect_process_waiting() -> bool {
		bool        ok                = true;
		const pid_t compare_child_pid = spawn_exiting_child(7);
		ok &= expect(compare_child_pid > 0, "spawns compare child");
		if (compare_child_pid > 0) {
			const int status = howdy::pam::compare_process::wait_until(
			    compare_child_pid, std::chrono::steady_clock::now() + std::chrono::seconds(1));
			ok &= expect(WIFEXITED(status) && WEXITSTATUS(status) == 7,
			             "compare wait preserves child exit status");
		}

		const pid_t helper_child_pid = spawn_exiting_child(9);
		ok &= expect(helper_child_pid > 0, "spawns helper child");
		if (helper_child_pid > 0) {
			const int status = howdy::pam::auth_helper_process::wait_for_helper(helper_child_pid);
			ok &= expect(WIFEXITED(status) && WEXITSTATUS(status) == 9,
			             "helper wait preserves child exit status");
		}

		constexpr pid_t kNonexistentChild = std::numeric_limits<pid_t>::max();
		const int       compare_failure   = howdy::pam::compare_process::wait_until(
		    kNonexistentChild, std::chrono::steady_clock::now() + std::chrono::seconds(1));
		const auto abort_code = static_cast<int>(howdy::native::CompareExit::kAbort);
		ok &= expect(WIFEXITED(compare_failure) && WEXITSTATUS(compare_failure) == abort_code,
		             "compare wait failure returns abort status");
		const int helper_failure =
		    howdy::pam::auth_helper_process::wait_for_helper(kNonexistentChild);
		ok &= expect(WIFEXITED(helper_failure) && WEXITSTATUS(helper_failure) == abort_code,
		             "helper wait failure returns abort status");

		return ok;
	}

	auto expect_auth_helper_output_limit_terminates_child() -> bool {
		bool                    ok = true;
		std::array<ScopedFd, 2> output_pipe;
		ok &= expect(open_pipe(&output_pipe), "creates output-limit auth-helper pipe");
		if (!ok) {
			return false;
		}

		const std::string limit_output(howdy::pam::auth_helper_process::output_limit(), 'h');
		const pid_t       child_pid = fork();
		ok &= expect(child_pid >= 0, "forks output-limit auth-helper child");
		if (child_pid < 0) {
			return false;
		}
		if (child_pid == 0) {
			output_pipe[0].reset();
			const bool wrote = write_all(output_pipe[1].get(), limit_output);
			usleep(2000000);
			_exit(wrote ? 0 : 1);
		}

		output_pipe[1].reset();
		std::string helper_output;
		const auto  start = std::chrono::steady_clock::now();
		const bool  helper_ok =
		    read_auth_helper_output(child_pid, output_pipe[0].get(), &helper_output);
		const auto elapsed = std::chrono::steady_clock::now() - start;

		ok &= expect(!helper_ok, "auth-helper output limit fails closed");
		ok &= expect(helper_output.empty(), "auth-helper output-limit data is not exposed");
		ok &= expect(elapsed < std::chrono::milliseconds(1500),
		             "auth-helper output limit returns before sleeping helper exits");

		int status              = 0;
		errno                   = 0;
		const pid_t wait_result = waitpid(child_pid, &status, WNOHANG);
		const int   wait_errno  = errno;
		ok &= expect(wait_result < 0 && wait_errno == ECHILD,
		             "output-limit auth-helper child is reaped");

		return ok;
	}

	auto expect_auth_helper_output_read_error_terminates_child() -> bool {
		bool        ok        = true;
		const pid_t child_pid = fork();
		ok &= expect(child_pid >= 0, "forks read-error auth-helper child");
		if (child_pid < 0) {
			return false;
		}
		if (child_pid == 0) {
			usleep(2000000);
			_exit(0);
		}

		std::string helper_output;
		const auto  start     = std::chrono::steady_clock::now();
		const bool  helper_ok = read_auth_helper_output(child_pid, -1, &helper_output);
		const auto  elapsed   = std::chrono::steady_clock::now() - start;

		ok &= expect(!helper_ok, "auth-helper read error fails closed");
		ok &= expect(helper_output.empty(), "auth-helper read error collects empty output");
		ok &= expect(elapsed < std::chrono::milliseconds(1500),
		             "auth-helper read error returns before sleeping helper exits");

		int   status      = 0;
		pid_t wait_result = 0;
		do {
			errno       = 0;
			wait_result = waitpid(child_pid, &status, WNOHANG);
		} while (wait_result < 0 && errno == EINTR);
		const int wait_errno = errno;
		ok &= expect(wait_result < 0 && wait_errno == ECHILD,
		             "read-error auth-helper child is reaped");

		if (wait_result == 0) {
			kill(child_pid, SIGKILL);
			do {
				errno       = 0;
				wait_result = waitpid(child_pid, &status, 0);
			} while (wait_result < 0 && errno == EINTR);
			ok &= expect(wait_result == child_pid,
			             "read-error auth-helper child cleanup reaps child");
		} else if (wait_result < 0 && wait_errno != ECHILD) {
			kill(child_pid, SIGKILL);
			do {
				errno       = 0;
				wait_result = waitpid(child_pid, &status, 0);
			} while (wait_result < 0 && errno == EINTR);
		}

		return ok;
	}

	auto expect_auth_helper_output_partial_read_error_discards_output() -> bool {
		bool        ok        = true;
		const pid_t child_pid = fork();
		ok &= expect(child_pid >= 0, "forks partial-read-error auth-helper child");
		if (child_pid < 0) {
			return false;
		}
		if (child_pid == 0) {
			usleep(2000000);
			_exit(0);
		}

		std::string helper_output = "previous output";
		const bool  helper_ok =
		    read_auth_helper_output(child_pid, -1, &helper_output, fake_partial_read_error);

		ok &= expect(!helper_ok, "auth-helper partial read error fails closed");
		ok &= expect(helper_output.empty(), "auth-helper partial read error discards output");

		int   status      = 0;
		pid_t wait_result = 0;
		do {
			errno       = 0;
			wait_result = waitpid(child_pid, &status, WNOHANG);
		} while (wait_result < 0 && errno == EINTR);
		const int wait_errno = errno;
		ok &= expect(wait_result < 0 && wait_errno == ECHILD,
		             "partial-read-error auth-helper child is reaped");
		if (wait_result == 0) {
			kill(child_pid, SIGKILL);
			do {
				errno       = 0;
				wait_result = waitpid(child_pid, &status, 0);
			} while (wait_result < 0 && errno == EINTR);
		}

		return ok;
	}

	auto read_clean_auth_helper_output(const std::string &child_output, std::string *read_output,
	                                   int child_exit_status = EXIT_SUCCESS)
	    -> std::optional<bool> {
		std::array<ScopedFd, 2> output_pipe;
		if (!open_pipe(&output_pipe)) {
			return std::nullopt;
		}

		const pid_t child_pid = fork();
		if (child_pid < 0) {
			return std::nullopt;
		}
		if (child_pid == 0) {
			output_pipe[0].reset();
			const bool wrote = write_all(output_pipe[1].get(), child_output);
			output_pipe[1].reset();
			_exit(wrote ? child_exit_status : EXIT_FAILURE);
		}

		output_pipe[1].reset();
		return read_auth_helper_output(child_pid, output_pipe[0].get(), read_output);
	}

	auto expect_auth_helper_output_child_failure_discards_output() -> bool {
		const std::string child_output  = "CONFIG_PATH=/run/howdy/config.ini\n"
		                                  "USER_MODELS_DIR=/run/howdy/models\n";
		std::string       actual_output = "previous output";
		const auto        read_result =
		    read_clean_auth_helper_output(child_output, &actual_output, EXIT_FAILURE);

		bool ok = true;
		ok &= expect(read_result.has_value(), "failed auth-helper child exits cleanly");
		if (!read_result.has_value()) {
			return false;
		}
		ok &= expect(!*read_result, "failed auth-helper child output is rejected");
		ok &= expect(actual_output.empty(), "failed auth-helper child output is discarded");
		return ok;
	}

	auto expect_auth_helper_output_protocol_validation() -> bool {
		struct ProtocolCase {
			std::string name;
			std::string output;
			bool        expected_ok;
			std::string config_path;
			std::string user_models_dir;
		};

		const std::vector<ProtocolCase> cases = {
		    {.name            = "valid required auth-helper output is accepted",
		     .output          = "CONFIG_PATH=/run/howdy/config.ini\n"
		                        "USER_MODELS_DIR=/run/howdy/models\n",
		     .expected_ok     = true,
		     .config_path     = "/run/howdy/config.ini",
		     .user_models_dir = "/run/howdy/models"},
		    {.name        = "duplicate CONFIG_PATH is rejected",
		     .output      = "CONFIG_PATH=/run/howdy/pam-1000-a/config.ini\n"
		                    "CONFIG_PATH=/run/howdy/pam-1000-b/config.ini\n"
		                    "USER_MODELS_DIR=/run/howdy/pam-1000-a/models\n",
		     .expected_ok = false},
		    {.name        = "duplicate USER_MODELS_DIR is rejected",
		     .output      = "CONFIG_PATH=/run/howdy/pam-1000-a/config.ini\n"
		                    "USER_MODELS_DIR=/run/howdy/pam-1000-a/models\n"
		                    "USER_MODELS_DIR=/run/howdy/pam-1000-b/models\n",
		     .expected_ok = false},
		    {.name        = "missing CONFIG_PATH is rejected",
		     .output      = "USER_MODELS_DIR=/run/howdy/models\n",
		     .expected_ok = false},
		    {.name        = "missing USER_MODELS_DIR is rejected",
		     .output      = "CONFIG_PATH=/run/howdy/config.ini\n",
		     .expected_ok = false},
		    {.name        = "empty CONFIG_PATH is rejected",
		     .output      = "CONFIG_PATH=\nUSER_MODELS_DIR=/run/howdy/models\n",
		     .expected_ok = false},
		    {.name        = "empty USER_MODELS_DIR is rejected",
		     .output      = "CONFIG_PATH=/run/howdy/config.ini\nUSER_MODELS_DIR=\n",
		     .expected_ok = false},
		    {.name        = "NOTICE line with valid required keys is rejected",
		     .output      = "NOTICE=ignored\n"
		                    "CONFIG_PATH=/run/howdy/config.ini\n"
		                    "USER_MODELS_DIR=/run/howdy/models\n",
		     .expected_ok = false},
		    {.name        = "unknown key with valid required keys is rejected",
		     .output      = "UNKNOWN=ignored\n"
		                    "CONFIG_PATH=/run/howdy/config.ini\n"
		                    "USER_MODELS_DIR=/run/howdy/models\n",
		     .expected_ok = false},
		    {.name        = "line without separator with valid required keys is rejected",
		     .output      = "CONFIG_PATH=/run/howdy/config.ini\n"
		                    "helper wrote stderr noise\n"
		                    "USER_MODELS_DIR=/run/howdy/models\n",
		     .expected_ok = false},
		    {.name            = "required value containing equals is preserved and accepted",
		     .output          = "CONFIG_PATH=/run/howdy/config=debug.ini\n"
		                        "USER_MODELS_DIR=/run/howdy/models=primary\n",
		     .expected_ok     = true,
		     .config_path     = "/run/howdy/config=debug.ini",
		     .user_models_dir = "/run/howdy/models=primary"},
		};

		bool ok = true;
		for (const auto &test_case : cases) {
			std::string actual_output;
			const auto  read_result =
			    read_clean_auth_helper_output(test_case.output, &actual_output);
			ok &= expect(read_result.has_value(), test_case.name + " helper exits cleanly");
			if (!read_result.has_value()) {
				continue;
			}

			ok &= expect(*read_result == test_case.expected_ok, test_case.name);
			if (!test_case.expected_ok) {
				ok &= expect(actual_output.empty(), test_case.name + " discards malformed output");
			}
			const auto parsed = howdy::pam::auth_helper_process::parse_output(test_case.output);
			ok &=
			    expect(parsed.valid == test_case.expected_ok, test_case.name + " parser validity");
			if (test_case.expected_ok) {
				ok &= expect(actual_output == test_case.output,
				             test_case.name + " exposes unchanged helper output");
				ok &= expect(parsed.config_path == test_case.config_path,
				             test_case.name + " config path parsed");
				ok &= expect(parsed.user_models_dir == test_case.user_models_dir,
				             test_case.name + " user models directory parsed");
			}
		}

		return ok;
	}

	auto expect_conversation_helpers() -> bool {
		using howdy::pam::auth_flow::auth_token_present;
		using howdy::pam::auth_flow::ConversationFn;
		using howdy::pam::auth_flow::make_conversation;
		using howdy::pam::auth_flow::send_conversation_message;

		bool                 ok            = true;
		int                  direct_calls  = 0;
		int                  direct_type   = 0;
		int                  direct_result = PAM_CONV_ERR;
		std::string          direct_message;
		const ConversationFn direct_conversation = [&](int msg_type, const char *message) -> int {
			++direct_calls;
			direct_type    = msg_type;
			direct_message = message == nullptr ? "" : message;
			return direct_result;
		};
		send_conversation_message(direct_conversation, PAM_ERROR_MSG, "direct message");
		ok &= expect(direct_calls == 1 && direct_type == PAM_ERROR_MSG &&
		                 direct_message == "direct message",
		             "message helper invokes conversation despite conversation failure");
		direct_result = PAM_SUCCESS;
		send_conversation_message(direct_conversation, PAM_TEXT_INFO, "successful message");
		ok &= expect(direct_calls == 2 && direct_type == PAM_TEXT_INFO &&
		                 direct_message == "successful message",
		             "message helper invokes successful conversation");

		ConversationState state;
		struct pam_conv   conversation{
		    .conv        = test_conversation,
		    .appdata_ptr = &state,
		};
		ScopedPamHandle pam_handle;
		ok &= expect(pam_handle.start(&conversation) == PAM_SUCCESS, "starts PAM handle");
		if (pam_handle.get() == nullptr) {
			return false;
		}

		ConversationFn wrapped_conversation;
		ok &= expect(make_conversation(pam_handle.get(), &wrapped_conversation) == PAM_SUCCESS,
		             "acquires PAM conversation");

		state.response_mode = ResponseMode::None;
		ok &= expect(wrapped_conversation(PAM_TEXT_INFO, "no response") == PAM_SUCCESS,
		             "wrapped conversation accepts null response");
		state.response_mode = ResponseMode::Empty;
		ok &= expect(wrapped_conversation(PAM_ERROR_MSG, "empty response") == PAM_SUCCESS,
		             "wrapped conversation frees response without text");
		state.response_mode = ResponseMode::Secret;
		state.result        = PAM_CONV_ERR;
		ok &= expect(wrapped_conversation(PAM_PROMPT_ECHO_OFF, "secret response") == PAM_CONV_ERR,
		             "wrapped conversation preserves callback result");
		ok &= expect(state.calls == 3 && state.last_msg_type == PAM_PROMPT_ECHO_OFF &&
		                 state.last_message == "secret response",
		             "wrapped conversation forwards message fields");

		ok &= expect(!auth_token_present(pam_handle.get()), "missing auth token is absent");

		struct pam_conv unavailable_conversation{
		    .conv        = nullptr,
		    .appdata_ptr = nullptr,
		};
		ok &= expect(pam_set_item(pam_handle.get(), PAM_CONV, &unavailable_conversation) ==
		                 PAM_SUCCESS,
		             "sets unavailable PAM conversation");
		ConversationFn unavailable_wrapper;
		ok &= expect(make_conversation(pam_handle.get(), &unavailable_wrapper) == PAM_SYSTEM_ERR,
		             "rejects unavailable PAM conversation callback");

		return ok;
	}

	auto expect_status_helpers() -> bool {
		using howdy::pam::auth_flow::ConversationFn;
		using howdy::pam::auth_flow::howdy_error;
		using howdy::pam::auth_flow::howdy_status;

		bool                 ok            = true;
		int                  calls         = 0;
		int                  last_msg_type = 0;
		std::string          last_message;
		const ConversationFn conversation = [&](int msg_type, const char *message) -> int {
			++calls;
			last_msg_type = msg_type;
			last_message  = message == nullptr ? "" : message;
			return PAM_SUCCESS;
		};

		const auto make_status = [](howdy::native::CompareExit exit_code) -> int {
			return static_cast<int>(exit_code) << 8;
		};

		ok &= expect(howdy_error(make_status(howdy::native::CompareExit::kTimeoutReached),
		                         conversation) == PAM_AUTH_ERR,
		             "timeout status fails closed");
		ok &= expect(calls == 1 && last_msg_type == PAM_ERROR_MSG &&
		                 last_message == "Failure, timeout reached",
		             "timeout status sends error conversation");

		calls = 0;
		ok &= expect(howdy_error(make_status(howdy::native::CompareExit::kNoFaceModel),
		                         conversation) == PAM_AUTH_ERR,
		             "no-model status fails closed");
		ok &= expect(calls == 0, "no-model status sends no conversation");
		ok &= expect(howdy_error(SIGTERM, conversation) == PAM_AUTH_ERR,
		             "signaled status fails closed");
		ok &= expect(calls == 0, "signaled status sends no conversation");
		ok &= expect(howdy_error(W_STOPCODE(SIGSTOP), conversation) == PAM_AUTH_ERR,
		             "stopped status fails closed");
		ok &= expect(calls == 0, "stopped status sends no conversation");

		howdy::native::RuntimeConfig confirmation_config;
		confirmation_config.core.no_confirmation = false;

		calls                = 0;
		std::string username = "alice";
		ok &= expect(howdy_status(username.data(), EXIT_SUCCESS, confirmation_config,
		                          conversation) == PAM_SUCCESS,
		             "successful status approves login");
		ok &= expect(calls == 1 && last_msg_type == PAM_TEXT_INFO &&
		                 last_message == "Identified face as alice",
		             "successful status sends enabled confirmation");

		howdy::native::RuntimeConfig quiet_config;
		quiet_config.core.no_confirmation = true;

		calls = 0;
		ok &= expect(howdy_status(username.data(), EXIT_SUCCESS, quiet_config, conversation) ==
		                 PAM_SUCCESS,
		             "quiet successful status approves login");
		ok &= expect(calls == 0, "quiet successful status sends no confirmation");

		ok &=
		    expect(howdy_status(username.data(), make_status(howdy::native::CompareExit::kTooDark),
		                        quiet_config, conversation) == PAM_AUTH_ERR,
		           "failed status delegates to error handling");
		ok &= expect(calls == 1 && last_msg_type == PAM_ERROR_MSG &&
		                 last_message == "Face detection image too dark",
		             "failed status sends mapped error conversation");

		return ok;
	}

	auto expect_authentication_preserves_host_locale_state() -> bool {
		using howdy::pam::PromptCoordinatorDependencies;
		using howdy::pam::RuntimeSessionDependencies;
		using howdy::pam::auth_flow::IdentifyDependencies;

		const auto prepare_runtime = [](void *, std::string_view,
		                                howdy::pam::PreparedRuntimeFiles *) -> bool {
			return false;
		};
		const auto cleanup_runtime = [](void *, const std::filesystem::path &) -> void {};
		const auto load_runtime_config =
		    [](void *,
		       const std::filesystem::path &path) -> howdy::native::RuntimeConfigLoadResult {
			howdy::native::RuntimeConfig config;
			config.core.detection_notice    = false;
			config.core.no_confirmation     = true;
			config.core.abort_if_ssh        = false;
			config.core.abort_if_lid_closed = false;
			config.core.disabled            = false;
			return howdy::native::RuntimeConfigLoadResult{
			    .ok            = true,
			    .status        = howdy::native::RuntimeConfigLoadStatus::kOk,
			    .path          = path,
			    .config        = std::move(config),
			    .error_message = {},
			    .error_code    = 0,
			};
		};
		const auto effective_uid = [](void *) -> uid_t {
			return 0;
		};
		const auto check_enabled = [](void *, const howdy::native::RuntimeConfig &, const char *,
		                              const std::filesystem::path &) -> int {
			return PAM_SUCCESS;
		};
		const auto spawn_compare = [](void *, const howdy::pam::CompareLaunchRequest &,
		                              pid_t *child_pid) -> int {
			*child_pid = 1;
			return 0;
		};
		const auto wait_compare = [](void *, pid_t, std::chrono::steady_clock::time_point) -> int {
			return 0;
		};
		const auto terminate_compare = [](void *, pid_t) -> void {};
		const auto input_preflight   = [](void *) -> bool {
			return true;
		};
		const auto create_enter_device = [](void *) -> std::unique_ptr<EnterDevice> {
			return nullptr;
		};
		const auto create_native_prompt = [](void *,
		                                     pam_handle_t *) -> std::unique_ptr<NativePrompt> {
			return nullptr;
		};
		const auto request_auth_token = [](void *, pam_handle_t *) -> std::tuple<int, char *> {
			return {PAM_SUCCESS, nullptr};
		};

		const RuntimeSessionDependencies runtime_dependencies{
		    .prepare_runtime     = prepare_runtime,
		    .cleanup_runtime     = cleanup_runtime,
		    .load_runtime_config = load_runtime_config,
		    .effective_uid       = effective_uid,
		};
		const PromptCoordinatorDependencies prompt_dependencies{
		    .spawn_compare_process    = spawn_compare,
		    .wait_for_compare_process = wait_compare,
		    .terminate_compare        = terminate_compare,
		    .input_prompt_preflight   = input_preflight,
		    .create_enter_device      = create_enter_device,
		    .create_native_prompt     = create_native_prompt,
		    .request_auth_token       = request_auth_token,
		};
		IdentifyDependencies dependencies{
		    .context            = nullptr,
		    .runtime_session    = runtime_dependencies,
		    .prompt_coordinator = prompt_dependencies,
		    .check_enabled      = check_enabled,
		};

		ConversationState state;
		struct pam_conv   conversation{
		    .conv        = test_conversation,
		    .appdata_ptr = &state,
		};
		ScopedPamHandle pam_handle;
		bool            ok = true;
		ok &= expect(pam_handle.start(&conversation) == PAM_SUCCESS,
		             "starts PAM handle for real authentication flow");
		if (pam_handle.get() == nullptr) {
			return false;
		}

		ScopedEnv         lc_all("LC_ALL");
		const char       *initial_locale_ptr = std::setlocale(LC_ALL, nullptr);
		const std::string initial_locale = initial_locale_ptr == nullptr ? "" : initial_locale_ptr;
		const char       *initial_domain_ptr = textdomain(nullptr);
		const std::string initial_domain = initial_domain_ptr == nullptr ? "" : initial_domain_ptr;
		const char       *host_environment_locale = "C.UTF-8";
		if (std::setlocale(LC_ALL, host_environment_locale) == nullptr) {
			host_environment_locale = "C.utf8";
		}
		const bool locale_available = std::setlocale(LC_ALL, host_environment_locale) != nullptr;
		if (locale_available) {
			setenv("LC_ALL", host_environment_locale, 1);
		}
		std::setlocale(LC_ALL, "C");
		textdomain("pam-host-test-domain");
		const std::string host_locale = std::setlocale(LC_ALL, nullptr);
		const std::string host_domain = textdomain(nullptr);

		const auto injected_identify = [](void *context, pam_handle_t *handle,
		                                  PamModuleArguments arguments, bool ask_auth_tok) -> int {
			const auto *injected = static_cast<const IdentifyDependencies *>(context);
			return howdy::pam::auth_flow::identify_with_dependencies(handle, arguments,
			                                                         ask_auth_tok, *injected);
		};
		ok &= expect(authenticate_with_identify(pam_handle.get(), {}, true, &dependencies,
		                                        injected_identify) == PAM_SUCCESS,
		             "real PAM authentication flow succeeds with injected boundaries");
		if (locale_available) {
			ok &= expect(std::string(std::setlocale(LC_ALL, nullptr)) == host_locale,
			             "real authentication preserves host locale");
		} else {
			std::cerr << "SKIP: C UTF-8 locale unavailable; locale mutation assertion not run\n";
		}
		ok &= expect(std::string(textdomain(nullptr)) == host_domain,
		             "real authentication preserves host gettext domain");

		ok &= expect(authenticate_with_identify(pam_handle.get(), {}, true, &dependencies,
		                                        injected_identify) == PAM_SUCCESS,
		             "repeated real PAM authentication flow succeeds");
		if (locale_available) {
			ok &= expect(std::string(std::setlocale(LC_ALL, nullptr)) == host_locale,
			             "repeated real authentication preserves host locale");
		}
		ok &= expect(std::string(textdomain(nullptr)) == host_domain,
		             "repeated real authentication preserves host gettext domain");

		if (!initial_domain.empty()) {
			textdomain(initial_domain.c_str());
		}
		if (!initial_locale.empty()) {
			std::setlocale(LC_ALL, initial_locale.c_str());
		}
		return ok;
	}

	auto expect_identify_dependency_validation() -> bool {
		bool ok = true;

		auto dependencies          = howdy::pam::auth_flow::production_identify_dependencies();
		dependencies.check_enabled = nullptr;
		ok &= expect(howdy::pam::auth_flow::identify_with_dependencies(
		                 nullptr, {}, true, dependencies) == PAM_SYSTEM_ERR,
		             "identify rejects missing enabled-check callback");

		dependencies = howdy::pam::auth_flow::production_identify_dependencies();
		dependencies.runtime_session.load_runtime_config = nullptr;
		ok &= expect(howdy::pam::auth_flow::identify_with_dependencies(
		                 nullptr, {}, true, dependencies) == PAM_SYSTEM_ERR,
		             "identify rejects missing runtime-session callback");

		dependencies = howdy::pam::auth_flow::production_identify_dependencies();
		dependencies.prompt_coordinator.spawn_compare_process = nullptr;
		ok &= expect(howdy::pam::auth_flow::identify_with_dependencies(
		                 nullptr, {}, true, dependencies) == PAM_SYSTEM_ERR,
		             "identify rejects missing prompt-coordinator callback");

		return ok;
	}

	auto expect_enabled_decisions() -> bool {
		using howdy::pam::auth_flow::check_enabled;

		bool ok = true;

		howdy::native::RuntimeConfig disabled_config;
		disabled_config.core.disabled = true;
		ok &= expect(check_enabled(disabled_config, "alice", "/") == PAM_AUTHINFO_UNAVAIL,
		             "disabled config skips authentication");

		const howdy::native::RuntimeConfig ssh_config;
		ScopedEnv                          ssh_connection("SSH_CONNECTION");
		setenv("SSH_CONNECTION", "client server", 1);
		ok &= expect(check_enabled(ssh_config, "alice", "/") == PAM_AUTHINFO_UNAVAIL,
		             "ssh environment skips authentication");
		unsetenv("SSH_CONNECTION");

		howdy::native::RuntimeConfig base_config;
		base_config.core.abort_if_ssh        = false;
		base_config.core.abort_if_lid_closed = false;

		ok &= expect(check_enabled(base_config, "../alice", "/") == PAM_AUTHINFO_UNAVAIL,
		             "invalid username skips authentication");
		ok &= expect(check_enabled(base_config, "howdy_missing_model_for_test", "/") ==
		                 PAM_AUTHINFO_UNAVAIL,
		             "missing model file skips authentication");
		ok &= expect(check_enabled(base_config, "alice", "/tmp") == PAM_AUTHINFO_UNAVAIL,
		             "insecure models directory skips authentication");

		if (geteuid() == 0) {
			namespace fs          = std::filesystem;
			const auto models_dir = create_temp_directory("/run/howdy-auth-flow-models-XXXXXX");
			ok &= expect(models_dir.has_value(), "creates root-owned models directory");
			if (models_dir.has_value()) {
				const fs::path model_path = *models_dir / "alice.dat";
				ok &= expect(chmod(models_dir->c_str(), 0755) == 0,
				             "sets secure models directory mode");
				ok &= expect(write_file(model_path, "[]"), "writes model file");
				ok &= expect(chmod(model_path.c_str(), 0644) == 0, "sets secure model file mode");
				ok &= expect(check_enabled(base_config, "alice", *models_dir) == PAM_SUCCESS,
				             "secure model path allows authentication");
				std::error_code ec;
				fs::remove_all(*models_dir, ec);
			}
		} else {
			std::cerr << "SKIP: check_enabled success path requires root-owned fixture\n";
		}

		return ok;
	}

	auto expect_prompt_stop_helpers() -> bool {
		using howdy::pam::request_password_prompt_stop;

		bool ok = true;

		optional_task<std::tuple<int, char *>> inactive_task([] -> std::tuple<int, char *> {
			return {PAM_SUCCESS, nullptr};
		});
		const PromptStopPlan                   no_stop_plan{
		    .stop_prompt  = false,
		    .abort_prompt = false,
		    .send_enter   = false,
		};
		const auto no_stop_result =
		    request_password_prompt_stop(inactive_task, no_stop_plan, nullptr, nullptr);
		ok &= expect(!no_stop_result.enter_failed && no_stop_result.prompt_stopped,
		             "no-stop plan returns default prompt stop result");
		ok &= expect(!inactive_task.active(), "no-stop plan leaves inactive task inactive");

		optional_task<std::tuple<int, char *>> ready_task([] -> std::tuple<int, char *> {
			return {PAM_SUCCESS, nullptr};
		});
		ready_task.activate();
		ok &= expect(ready_task.wait(std::chrono::seconds(1)) == std::future_status::ready,
		             "ready prompt task finishes before stop");
		const PromptStopPlan stop_plan{
		    .stop_prompt  = true,
		    .abort_prompt = false,
		    .send_enter   = false,
		};
		const auto stop_result =
		    request_password_prompt_stop(ready_task, stop_plan, nullptr, nullptr);
		ok &= expect(!stop_result.enter_failed && stop_result.prompt_stopped,
		             "stop plan stops ready prompt without input");
		ok &= expect(!ready_task.active(), "stop plan deactivates ready prompt task");
		ok &= expect(std::get<0>(ready_task.get()) == PAM_SUCCESS,
		             "stopped ready prompt keeps task result");

		optional_task<std::tuple<int, char *>> abort_without_native_prompt(
		    [] -> std::tuple<int, char *> {
			    return {PAM_CONV_ERR, nullptr};
		    });
		abort_without_native_prompt.activate();
		ok &= expect(abort_without_native_prompt.wait(std::chrono::seconds(1)) ==
		                 std::future_status::ready,
		             "abort prompt task finishes before stop");
		const PromptStopPlan abort_plan{
		    .stop_prompt  = true,
		    .abort_prompt = true,
		    .send_enter   = false,
		};
		const auto abort_result =
		    request_password_prompt_stop(abort_without_native_prompt, abort_plan, nullptr, nullptr);
		ok &= expect(!abort_result.enter_failed && abort_result.prompt_stopped,
		             "abort plan without native prompt stops task safely");
		ok &= expect(!abort_without_native_prompt.active(),
		             "abort plan deactivates prompt task without native prompt");

		optional_task<std::tuple<int, char *>> ready_input_task([] -> std::tuple<int, char *> {
			return {PAM_SUCCESS, nullptr};
		});
		ready_input_task.activate();
		ok &= expect(ready_input_task.wait(std::chrono::seconds(1)) == std::future_status::ready,
		             "ready input prompt task finishes before stop");
		const PromptStopPlan input_plan{
		    .stop_prompt  = true,
		    .abort_prompt = false,
		    .send_enter   = true,
		};
		const auto input_result =
		    request_password_prompt_stop(ready_input_task, input_plan, nullptr, nullptr);
		ok &= expect(!input_result.enter_failed && input_result.prompt_stopped,
		             "already-ready input prompt skips Enter injection");
		ok &=
		    expect(!ready_input_task.active(), "input plan deactivates already-ready prompt task");

		return ok;
	}

}  // namespace

auto main() -> int {
	using namespace howdy::native::auth_helper_protocol;

	bool ok = true;

	ok &= expect_fd_reading();
	ok &= expect_process_waiting();
	ok &= expect_auth_helper_output_limit_terminates_child();
	ok &= expect_auth_helper_output_read_error_terminates_child();
	ok &= expect_auth_helper_output_partial_read_error_discards_output();
	ok &= expect_auth_helper_output_child_failure_discards_output();
	ok &= expect_auth_helper_output_protocol_validation();
	ok &= expect_conversation_helpers();
	ok &= expect_status_helpers();
	ok &= expect_authentication_preserves_host_locale_state();
	ok &= expect_identify_dependency_validation();
	ok &= expect_enabled_decisions();
	ok &= expect_prompt_stop_helpers();

	ok &= expect(std::string(kConfigPathKey) == "CONFIG_PATH",
	             "config path protocol key remains unchanged");
	ok &= expect(std::string(kUserModelsDirKey) == "USER_MODELS_DIR",
	             "user models directory protocol key remains unchanged");

	return ok ? 0 : 1;
}
