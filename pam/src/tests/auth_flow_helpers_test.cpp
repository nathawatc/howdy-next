#include "auth_flow_testing.hpp"
#include "common/compare_exit.hpp"
#include "config/config_reader.hpp"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <unistd.h>

#include <security/pam_appl.h>

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
        std::size_t offset = 0;
        while (offset < data.size()) {
            const ssize_t result = write(fd, data.data() + offset, data.size() - offset);
            if (result < 0 && errno == EINTR) {
                continue;
            }
            if (result <= 0) {
                return false;
            }
            offset += static_cast<std::size_t>(result);
        }
        return true;
    }

    auto write_file(const std::string &path, const std::string &content) -> bool {
        std::ofstream output(path);
        output << content;
        return output.good();
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
        ok &= expect(write_all(small_pipe[1].get(), "CONFIG_PATH=/run/howdy/config.ini\n"),
                     "writes small helper output");
        small_pipe[1].reset();
        ok &=
            expect(read_fd_to_string(small_pipe[0].get()) == "CONFIG_PATH=/run/howdy/config.ini\n",
                   "reads complete small helper output");

        std::string temp_path = "/tmp/howdy-auth-flow-output-XXXXXX";
        ScopedFd    bounded_fd(mkstemp(temp_path.data()));
        unlink(temp_path.c_str());
        ok &= expect(bounded_fd.get() >= 0, "creates bounded input file");
        const std::string oversized_output(16384, 'x');
        ok &=
            expect(write_all(bounded_fd.get(), oversized_output), "writes oversized helper output");
        ok &= expect(lseek(bounded_fd.get(), 0, SEEK_SET) == 0, "rewinds oversized helper output");
        const std::string bounded_output = read_fd_to_string(bounded_fd.get());
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
        ok &= expect(!auth_token_present(nullptr), "invalid PAM handle reports absent auth token");

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
        ok &= expect(make_conversation(nullptr, &unavailable_wrapper) != PAM_SUCCESS,
                     "propagates PAM conversation acquisition failure");

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

        std::string confirmation_path = "/tmp/howdy-auth-flow-confirmation-XXXXXX";
        ScopedFd    confirmation_fd(mkstemp(confirmation_path.data()));
        ok &= expect(confirmation_fd.get() >= 0, "creates confirmation config");
        confirmation_fd.reset();
        ok &= expect(write_file(confirmation_path, "[core]\nno_confirmation = false\n"),
                     "writes confirmation config");
        const howdy::native::ConfigReader confirmation_config(confirmation_path);
        unlink(confirmation_path.c_str());
        ok &= expect(confirmation_config.ok(), "parses confirmation config");

        calls                = 0;
        std::string username = "alice";
        ok &= expect(howdy_status(username.data(), EXIT_SUCCESS, confirmation_config,
                                  conversation) == PAM_SUCCESS,
                     "successful status approves login");
        ok &= expect(calls == 1 && last_msg_type == PAM_TEXT_INFO &&
                         last_message == "Identified face as alice",
                     "successful status sends enabled confirmation");

        std::string quiet_path = "/tmp/howdy-auth-flow-quiet-XXXXXX";
        ScopedFd    quiet_fd(mkstemp(quiet_path.data()));
        ok &= expect(quiet_fd.get() >= 0, "creates quiet config");
        quiet_fd.reset();
        ok &= expect(write_file(quiet_path, "[core]\nno_confirmation = true\n"),
                     "writes quiet config");
        const howdy::native::ConfigReader quiet_config(quiet_path);
        unlink(quiet_path.c_str());
        ok &= expect(quiet_config.ok(), "parses quiet config");

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

}  // namespace

auto main() -> int {
    using howdy::pam::testing::helper_output_value;

    bool ok = true;

    ok &= expect_fd_reading();
    ok &= expect_process_waiting();
    ok &= expect_conversation_helpers();
    ok &= expect_status_helpers();

    const std::string output =
        "NOTICE=ignored\nCONFIG_PATH=/run/howdy/config.ini\nUSER_MODELS_DIR=/run/howdy/models\n";
    ok &= expect(helper_output_value(output, "CONFIG_PATH") == "/run/howdy/config.ini",
                 "extracts config path");
    ok &= expect(helper_output_value(output, "USER_MODELS_DIR") == "/run/howdy/models",
                 "extracts user models directory");
    ok &= expect(helper_output_value("CONFIG_PATH=/run/howdy=config.ini\n", "CONFIG_PATH") ==
                     "/run/howdy=config.ini",
                 "preserves equals characters in value");
    ok &= expect(helper_output_value("CONFIG_PATH_EXTRA=wrong\nCONFIG_PATH=right", "CONFIG_PATH") ==
                     "right",
                 "matches exact key and parses final line");
    ok &= expect(helper_output_value(output, "MISSING").empty(), "missing key returns empty value");
    ok &= expect(helper_output_value("CONFIG_PATH=\n", "CONFIG_PATH").empty(),
                 "empty helper value remains empty");

    return ok ? 0 : 1;
}
