#include "auth_flow_testing.hpp"
#include "common/auth_helper_protocol.hpp"
#include "common/compare_exit.hpp"
#include "common/fd_io.hpp"
#include "config/runtime_config.hpp"

#include <array>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
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

	enum class ResponseMode {
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

	auto expect(bool condition, const std::string &message) -> bool {
		if (!condition) {
			std::cerr << "FAIL: " << message << "\n";
			return false;
		}
		return true;
	}

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

	auto write_file(const std::string &path, const std::string &content) -> bool {
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
		using howdy::pam::testing::read_fd_to_string;

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
		const std::string oversized_output(16384, 'x');
		ok &= expect(write_all(bounded_file->fd.get(), oversized_output),
		             "writes oversized helper output");
		ok &= expect(lseek(bounded_file->fd.get(), 0, SEEK_SET) == 0,
		             "rewinds oversized helper output");
		const std::string bounded_output = read_fd_to_string(bounded_file->fd.get());
		ok &= expect(bounded_output == oversized_output.substr(0, 9216),
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
		using howdy::pam::testing::wait_for_compare_process;
		using howdy::pam::testing::wait_for_helper_process;

		bool        ok                = true;
		const pid_t compare_child_pid = spawn_exiting_child(7);
		ok &= expect(compare_child_pid > 0, "spawns compare child");
		if (compare_child_pid > 0) {
			const int status = wait_for_compare_process(compare_child_pid);
			ok &= expect(WIFEXITED(status) && WEXITSTATUS(status) == 7,
			             "compare wait preserves child exit status");
		}

		const pid_t helper_child_pid = spawn_exiting_child(9);
		ok &= expect(helper_child_pid > 0, "spawns helper child");
		if (helper_child_pid > 0) {
			const int status = wait_for_helper_process(helper_child_pid);
			ok &= expect(WIFEXITED(status) && WEXITSTATUS(status) == 9,
			             "helper wait preserves child exit status");
		}

		constexpr pid_t kNonexistentChild = std::numeric_limits<pid_t>::max();
		const int       compare_failure   = wait_for_compare_process(kNonexistentChild);
		const auto      abort_code        = static_cast<int>(howdy::native::CompareExit::kAbort);
		ok &= expect(WIFEXITED(compare_failure) && WEXITSTATUS(compare_failure) == abort_code,
		             "compare wait failure returns abort status");
		const int helper_failure = wait_for_helper_process(kNonexistentChild);
		ok &= expect(WIFEXITED(helper_failure) && WEXITSTATUS(helper_failure) == abort_code,
		             "helper wait failure returns abort status");

		return ok;
	}

	auto expect_conversation_helpers() -> bool {
		using howdy::pam::testing::auth_token_present;
		using howdy::pam::testing::ConversationFn;
		using howdy::pam::testing::make_conversation;
		using howdy::pam::testing::send_conversation_message;

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
		using howdy::pam::testing::ConversationFn;
		using howdy::pam::testing::howdy_error;
		using howdy::pam::testing::howdy_status;

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

		const auto make_status = [](howdy::native::CompareExit exit_code) {
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

		const howdy::native::RuntimeConfig quiet_config;

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

	auto expect_enabled_decisions() -> bool {
		using howdy::pam::testing::check_enabled;

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
				ok &= expect(write_file(model_path.string(), "[]"), "writes model file");
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
		using howdy::pam::testing::request_password_prompt_stop;

		bool ok = true;

		optional_task<std::tuple<int, char *>> inactive_task([] {
			return std::tuple<int, char *>(PAM_SUCCESS, nullptr);
		});
		const PromptStopPlan                   no_stop_plan{
		    .stop_prompt  = false,
		    .abort_prompt = false,
		    .send_enter   = false,
		};
		const auto no_stop_result = request_password_prompt_stop(inactive_task, no_stop_plan);
		ok &= expect(!no_stop_result.enter_failed && no_stop_result.prompt_stopped,
		             "no-stop plan returns default prompt stop result");
		ok &= expect(!inactive_task.active(), "no-stop plan leaves inactive task inactive");

		optional_task<std::tuple<int, char *>> ready_task([] {
			return std::tuple<int, char *>(PAM_SUCCESS, nullptr);
		});
		ready_task.activate();
		ok &= expect(ready_task.wait(std::chrono::seconds(1)) == std::future_status::ready,
		             "ready prompt task finishes before stop");
		const PromptStopPlan stop_plan{
		    .stop_prompt  = true,
		    .abort_prompt = false,
		    .send_enter   = false,
		};
		const auto stop_result = request_password_prompt_stop(ready_task, stop_plan);
		ok &= expect(!stop_result.enter_failed && stop_result.prompt_stopped,
		             "stop plan stops ready prompt without input");
		ok &= expect(!ready_task.active(), "stop plan deactivates ready prompt task");
		ok &= expect(std::get<0>(ready_task.get()) == PAM_SUCCESS,
		             "stopped ready prompt keeps task result");

		optional_task<std::tuple<int, char *>> abort_without_native_prompt([] {
			return std::tuple<int, char *>(PAM_CONV_ERR, nullptr);
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
		    request_password_prompt_stop(abort_without_native_prompt, abort_plan);
		ok &= expect(!abort_result.enter_failed && abort_result.prompt_stopped,
		             "abort plan without native prompt stops task safely");
		ok &= expect(!abort_without_native_prompt.active(),
		             "abort plan deactivates prompt task without native prompt");

		if (euidaccess("/dev/uinput", W_OK | R_OK) != 0) {
			optional_task<std::tuple<int, char *>> input_task([] {
				return std::tuple<int, char *>(PAM_SUCCESS, nullptr);
			});
			input_task.activate();
			ok &= expect(input_task.wait(std::chrono::seconds(1)) == std::future_status::ready,
			             "input prompt task finishes before stop");
			const PromptStopPlan input_plan{
			    .stop_prompt  = true,
			    .abort_prompt = false,
			    .send_enter   = true,
			};
			const auto input_result = request_password_prompt_stop(input_task, input_plan);
			ok &= expect(input_result.enter_failed && input_result.prompt_stopped,
			             "input plan reports enter failure when uinput is unavailable");
			ok &= expect(!input_task.active(), "input plan deactivates ready prompt task");
		} else {
			std::cerr << "SKIP: prompt stop input failure requires unavailable /dev/uinput\n";
		}

		return ok;
	}

}  // namespace

auto main() -> int {
	using namespace howdy::native::auth_helper_protocol;
	using howdy::pam::testing::helper_output_value;

	bool ok = true;

	ok &= expect_fd_reading();
	ok &= expect_process_waiting();
	ok &= expect_conversation_helpers();
	ok &= expect_status_helpers();
	ok &= expect_enabled_decisions();
	ok &= expect_prompt_stop_helpers();

	ok &= expect(std::string(kConfigPathKey) == "CONFIG_PATH",
	             "config path protocol key remains unchanged");
	ok &= expect(std::string(kUserModelsDirKey) == "USER_MODELS_DIR",
	             "user models directory protocol key remains unchanged");

	const std::string output =
	    "NOTICE=ignored\nCONFIG_PATH=/run/howdy/config.ini\nUSER_MODELS_DIR=/run/howdy/models\n";
	ok &= expect(helper_output_value(output, kConfigPathKey) == "/run/howdy/config.ini",
	             "extracts config path");
	ok &= expect(helper_output_value(output, kUserModelsDirKey) == "/run/howdy/models",
	             "extracts user models directory");
	ok &= expect(helper_output_value("CONFIG_PATH=/run/howdy=config.ini\n", kConfigPathKey) ==
	                 "/run/howdy=config.ini",
	             "preserves equals characters in value");
	ok &= expect(helper_output_value("CONFIG_PATH_EXTRA=wrong\nCONFIG_PATH=right",
	                                 kConfigPathKey) == "right",
	             "matches exact key and parses final line");
	ok &= expect(helper_output_value(output, "MISSING").empty(), "missing key returns empty value");
	ok &= expect(helper_output_value("CONFIG_PATH=\n", kConfigPathKey).empty(),
	             "empty helper value remains empty");

	return ok ? 0 : 1;
}
