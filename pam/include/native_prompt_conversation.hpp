#ifndef HOWDY_PAM_NATIVE_PROMPT_CONVERSATION_HPP
#define HOWDY_PAM_NATIVE_PROMPT_CONVERSATION_HPP

#include <array>
#include <atomic>

#include <security/pam_appl.h>

class NativePromptConversation {
public:
    explicit NativePromptConversation(pam_handle_t *pamh);
    ~NativePromptConversation();

    NativePromptConversation(const NativePromptConversation &)                     = delete;
    auto operator=(const NativePromptConversation &) -> NativePromptConversation & = delete;

#ifdef HOWDY_PAM_TESTING
    NativePromptConversation(int tty_fd, int abort_read_fd, int abort_write_fd);
    void set_test_throw_mode(int mode);
    void set_test_poll_eintr_count(int count);
    void set_test_read_eintr_count(int count);
    void set_test_abort_on_poll_eintr(bool enabled);
    void set_test_abort_on_read_eintr(bool enabled);
#endif

    [[nodiscard]] auto available() const -> bool;
    auto               install() -> int;
    void               request_abort();
    void               restore_original();

private:
    static auto dispatch(int num_msg, const struct pam_message **msgm,
                         struct pam_response **response, void *appdata_ptr) -> int;
    auto        handle(int num_msg, const struct pam_message **msgm, struct pam_response **response)
        -> int;
    [[nodiscard]] auto write_message_line(const struct pam_message &message) const -> int;
    auto prompt_input(const struct pam_message &message, char **response, bool hide_input) -> int;

    pam_handle_t      *pamh_ = nullptr;
    struct pam_conv    original_conv_{};
    struct pam_conv    override_conv_{};
    bool               has_original_conv_ = false;
    bool               installed_         = false;
    int                tty_fd_            = -1;
    std::array<int, 2> abort_pipe_{{-1, -1}};
    std::atomic<bool>  abort_requested_{false};
#ifdef HOWDY_PAM_TESTING
    int  test_throw_mode_          = 0;
    int  test_poll_eintr_count_    = 0;
    int  test_read_eintr_count_    = 0;
    bool test_abort_on_poll_eintr_ = false;
    bool test_abort_on_read_eintr_ = false;
#endif
};

#endif  // HOWDY_PAM_NATIVE_PROMPT_CONVERSATION_HPP
