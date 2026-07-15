#ifndef HOWDY_PAM_NATIVE_PROMPT_CONVERSATION_HPP
#define HOWDY_PAM_NATIVE_PROMPT_CONVERSATION_HPP

#include <array>
#include <atomic>
#include <poll.h>
#include <termios.h>

#include <security/pam_appl.h>

#include <sys/types.h>

class NativePromptConversation {
public:
	explicit NativePromptConversation(pam_handle_t *pamh);
	~NativePromptConversation();

	NativePromptConversation(const NativePromptConversation &)                     = delete;
	auto operator=(const NativePromptConversation &) -> NativePromptConversation & = delete;

#ifdef HOWDY_PAM_TESTING
	struct TestDescriptors {
		int tty_fd         = -1;
		int abort_read_fd  = -1;
		int abort_write_fd = -1;
	};

	explicit NativePromptConversation(TestDescriptors descriptors);
	void set_test_throw_mode(int mode);
	void set_test_poll_eintr_count(int count);
	void set_test_read_eintr_count(int count);
	void set_test_abort_on_poll_eintr(bool enabled);
	void set_test_abort_on_read_eintr(bool enabled);
	void set_test_restore_failure(bool enabled);

	static void set_test_available_result(int result);
	static void set_test_install_result(int result);
#endif

	[[nodiscard]] auto available() const -> bool;
	auto               install() -> int;
	void               request_abort();
	void               restore_original();

private:
	enum class PromptIoResult : std::uint8_t {
		retry,
		ready,
		abort,
	};

	static auto dispatch(int num_msg, const struct pam_message **msgm,
	                     struct pam_response **response, void *appdata_ptr) -> int;
	auto        handle(int num_msg, const struct pam_message **msgm, struct pam_response **response)
	    -> int;
	[[nodiscard]] auto write_message_line(const struct pam_message &message) const -> int;
	auto prompt_input(const struct pam_message &message, char **response, bool hide_input) -> int;
#ifdef HOWDY_PAM_TESTING
	auto poll_prompt(std::array<struct pollfd, 2> &fds) -> int;
#else
	static auto poll_prompt(std::array<struct pollfd, 2> &fds) -> int;
#endif
#ifdef HOWDY_PAM_TESTING
	auto read_prompt_char(char *ch) -> ssize_t;
#else
	auto read_prompt_char(char *ch) const -> ssize_t;
#endif
	auto               poll_prompt_state(std::array<struct pollfd, 2> &fds) -> PromptIoResult;
	auto               read_prompt_state(char *ch) -> PromptIoResult;
	auto               wait_for_prompt_character(char *ch) -> PromptIoResult;
	[[nodiscard]] auto restore_prompt_terminal(const struct termios &original_termios) const
	    -> bool;

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
	bool test_restore_failure_     = false;
#endif
};

#endif  // HOWDY_PAM_NATIVE_PROMPT_CONVERSATION_HPP
