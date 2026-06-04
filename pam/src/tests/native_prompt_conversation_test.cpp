#ifndef HOWDY_PAM_TESTING
#    define HOWDY_PAM_TESTING
#endif

#define private public
#include "native_prompt_conversation.hpp"
#undef private

#include <array>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <string>
#include <termios.h>
#include <thread>
#include <unistd.h>

#include <security/pam_appl.h>

namespace {

    constexpr int kPromptReadTimeoutMs = 1000;

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

        [[nodiscard]] auto valid() const -> bool {
            return fd_ >= 0;
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

    auto expect(bool condition, const std::string &message) -> bool {
        if (!condition) {
            std::cerr << "FAIL: " << message << "\n";
            return false;
        }
        return true;
    }

    auto open_pty_pair(ScopedFd *master_fd, ScopedFd *slave_fd) -> bool {
        master_fd->reset(posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC));
        if (!master_fd->valid()) {
            return false;
        }

        if (grantpt(master_fd->get()) != 0 || unlockpt(master_fd->get()) != 0) {
            master_fd->reset();
            return false;
        }

        char *slave_name = ptsname(master_fd->get());
        if (slave_name == nullptr) {
            master_fd->reset();
            return false;
        }

        slave_fd->reset(open(slave_name, O_RDWR | O_NOCTTY | O_CLOEXEC));
        if (!slave_fd->valid()) {
            master_fd->reset();
            return false;
        }

        return true;
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

    auto read_with_timeout(int fd, char *buffer, std::size_t buffer_size, int timeout_ms)
        -> ssize_t {
        struct pollfd poll_fd{.fd = fd, .events = POLLIN, .revents = 0};

        while (true) {
            const int poll_result = poll(&poll_fd, 1, timeout_ms);
            if (poll_result < 0 && errno == EINTR) {
                continue;
            }
            if (poll_result == 0) {
                return 0;
            }
            if (poll_result < 0 || (poll_fd.revents & POLLIN) == 0) {
                return -1;
            }

            while (true) {
                const ssize_t bytes_read = read(fd, buffer, buffer_size);
                if (bytes_read < 0 && errno == EINTR) {
                    continue;
                }
                return bytes_read;
            }
        }
    }

    auto test_conv(int /*num_msg*/, const struct pam_message ** /*msgm*/,
                   struct pam_response **response, void *appdata_ptr) -> int {
        if (response != nullptr) {
            *response = nullptr;
        }
        return appdata_ptr == nullptr ? PAM_CONV_ERR : PAM_SUCCESS;
    }

    auto expect_dispatch_rejects_invalid_state() -> bool {
        bool ok = true;

        const struct pam_message message = {
            .msg_style = PAM_TEXT_INFO,
            .msg       = "notice",
        };
        const struct pam_message *message_ptr = &message;
        auto                     *responses   = reinterpret_cast<struct pam_response *>(0x1);

        ok &= expect(NativePromptConversation::dispatch(1, &message_ptr, &responses, nullptr) ==
                         PAM_CONV_ERR,
                     "dispatch rejects null appdata");
        ok &= expect(responses == nullptr, "dispatch clears response on null appdata");
        ok &= expect(NativePromptConversation::dispatch(1, &message_ptr, nullptr, nullptr) ==
                         PAM_CONV_ERR,
                     "dispatch rejects null response pointer");

        return ok;
    }

    auto expect_original_conversation_restored() -> bool {
        bool ok = true;

        ScopedFd                master_fd;
        ScopedFd                slave_fd;
        std::array<ScopedFd, 2> abort_pipe;
        ok &= expect(open_pty_pair(&master_fd, &slave_fd), "restore test opens pseudo terminal");
        ok &= expect(open_pipe(&abort_pipe), "restore test creates abort pipe");
        if (!ok) {
            return false;
        }

        int             appdata = 42;
        struct pam_conv original_conv{
            .conv        = test_conv,
            .appdata_ptr = &appdata,
        };
        pam_handle_t *pamh = nullptr;
        if (pam_start("howdy-native-test", "test-user", &original_conv, &pamh) != PAM_SUCCESS ||
            pamh == nullptr) {
            return expect(false, "restore test starts PAM handle");
        }

        {
            NativePromptConversation conversation(pamh);
            conversation.tty_fd_        = slave_fd.release();
            conversation.abort_pipe_[0] = abort_pipe[0].release();
            conversation.abort_pipe_[1] = abort_pipe[1].release();
            ok &= expect(conversation.available(), "restore test native prompt is available");
            ok &= expect(conversation.install() == PAM_SUCCESS,
                         "restore test installs native conversation");
        }

        const void *restored_item = nullptr;
        ok &= expect(pam_get_item(pamh, PAM_CONV, &restored_item) == PAM_SUCCESS,
                     "restore test reads PAM conversation");
        const auto *restored_conv = static_cast<const struct pam_conv *>(restored_item);
        ok &= expect(restored_conv != nullptr, "restore test returns restored PAM conversation");
        ok &= expect(restored_conv->conv == original_conv.conv,
                     "restore test restores original PAM conversation callback");
        ok &= expect(restored_conv->appdata_ptr == original_conv.appdata_ptr,
                     "restore test restores original PAM conversation appdata");
        pam_end(pamh, PAM_SUCCESS);
        return ok;
    }

    auto expect_dispatch_throw_cleanup(int throw_mode, const std::string &message) -> bool {
        bool                    ok = true;
        ScopedFd                master_fd;
        ScopedFd                slave_fd;
        std::array<ScopedFd, 2> abort_pipe;

        ok &= expect(open_pty_pair(&master_fd, &slave_fd), message + ": opens pseudo terminal");
        ok &= expect(open_pipe(&abort_pipe), message + ": creates abort pipe");
        if (!ok) {
            return false;
        }

        NativePromptConversation conversation(slave_fd.release(), abort_pipe[0].release(),
                                              abort_pipe[1].release());
        conversation.set_test_throw_mode(throw_mode);

        const struct pam_message prompt = {
            .msg_style = PAM_PROMPT_ECHO_OFF,
            .msg       = "Password: ",
        };
        const struct pam_message *prompt_ptr      = &prompt;
        auto                     *responses       = reinterpret_cast<struct pam_response *>(0x1);
        int                       dispatch_result = PAM_SUCCESS;

        std::thread dispatch_thread([&] {
            dispatch_result =
                NativePromptConversation::dispatch(1, &prompt_ptr, &responses, &conversation);
        });

        std::array<char, 64> prompt_buffer{};
        const ssize_t prompt_bytes = read_with_timeout(master_fd.get(), prompt_buffer.data(),
                                                       prompt_buffer.size(), kPromptReadTimeoutMs);
        ok &= expect(prompt_bytes > 0, message + ": prompt is written to tty");

        constexpr std::array<char, 7> kPassword{'s', 'e', 'c', 'r', 'e', 't', '\n'};
        ok &= expect(write(master_fd.get(), kPassword.data(), kPassword.size()) ==
                         static_cast<ssize_t>(kPassword.size()),
                     message + ": writes password response");

        dispatch_thread.join();

        ok &= expect(dispatch_result == PAM_CONV_ERR, message + ": dispatch fails closed");
        ok &= expect(responses == nullptr, message + ": dispatch resets response");

        if (responses != nullptr) {
            std::free(responses);
        }
        return ok;
    }

}  // namespace

auto main() -> int {
    bool ok = true;

    ScopedFd                master_fd;
    ScopedFd                slave_fd;
    std::array<ScopedFd, 2> abort_pipe;

    ok &= expect(open_pty_pair(&master_fd, &slave_fd), "opens pseudo terminal");
    ok &= expect(open_pipe(&abort_pipe), "creates abort pipe");
    if (!ok) {
        return 1;
    }

    const struct pam_message message = {
        .msg_style = PAM_PROMPT_ECHO_OFF,
        .msg       = "Password: ",
    };

    const int                slave_raw_fd = slave_fd.get();
    NativePromptConversation conversation(slave_fd.release(), abort_pipe[0].release(),
                                          abort_pipe[1].release());

    int         prompt_result = PAM_SUCCESS;
    char       *response      = nullptr;
    std::thread prompt_thread([&] {
        prompt_result = conversation.prompt_input(message, &response, true);
    });

    std::array<char, 64> prompt_buffer{};
    const ssize_t prompt_bytes = read_with_timeout(master_fd.get(), prompt_buffer.data(),
                                                   prompt_buffer.size(), kPromptReadTimeoutMs);
    ok &= expect(prompt_bytes > 0, "prompt is written to tty");

    constexpr char kCtrlC = 3;
    ok &= expect(write(master_fd.get(), &kCtrlC, 1) == 1, "writes Ctrl-C byte to pseudo terminal");

    prompt_thread.join();

    ok &= expect(prompt_result == PAM_CONV_ERR, "Ctrl-C byte aborts the native prompt");
    ok &= expect(response == nullptr, "aborted prompt does not return a response");

    struct termios restored_termios{};
    ok &= expect(tcgetattr(slave_raw_fd, &restored_termios) == 0,
                 "terminal state remains readable after abort");
    ok &= expect((restored_termios.c_lflag & ICANON) != 0, "canonical mode restored after abort");
    ok &= expect((restored_termios.c_lflag & ECHO) != 0, "echo restored after abort");
    ok &= expect((restored_termios.c_lflag & ISIG) != 0, "signal generation restored after abort");

    if (response != nullptr) {
        std::free(response);
    }

    ok &= expect_dispatch_rejects_invalid_state();
    ok &= expect_original_conversation_restored();
    ok &= expect_dispatch_throw_cleanup(1, "std exception after response allocation");
    ok &= expect_dispatch_throw_cleanup(2, "unknown exception after response allocation");

    return ok ? 0 : 1;
}
