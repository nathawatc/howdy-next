#ifndef HOWDY_PAM_NATIVE_PROMPT_CONVERSATION_HPP
#define HOWDY_PAM_NATIVE_PROMPT_CONVERSATION_HPP

#include <array>
#include <atomic>
#include <cstddef>
#include <poll.h>
#include <termios.h>

#include <security/pam_appl.h>

#include <sys/types.h>

class NativePromptConversationTestAccess;

class NativePrompt {
public:
	virtual ~NativePrompt() = default;

	[[nodiscard]] virtual auto available() const -> bool = 0;
	virtual auto               install() -> int          = 0;
	virtual void               request_abort()           = 0;
	virtual void               restore_original()        = 0;
};

class NativePromptConversation final : public NativePrompt {
public:
	explicit NativePromptConversation(pam_handle_t *pamh);
	~NativePromptConversation() override;

	NativePromptConversation(const NativePromptConversation &)                     = delete;
	auto operator=(const NativePromptConversation &) -> NativePromptConversation & = delete;

	[[nodiscard]] auto available() const -> bool override;
	auto               install() -> int override;
	void               request_abort() override;
	void               restore_original() override;

private:
	friend class NativePromptConversationTestAccess;

	struct Operations {
		void *context                                                                    = nullptr;
		int (*poll_prompt)(void *context, struct pollfd *fds, nfds_t count, int timeout) = nullptr;
		ssize_t (*read_prompt)(void *context, int fd, void *buffer, std::size_t count)   = nullptr;
		int (*restore_terminal)(void *context, int fd, const struct termios *termios)    = nullptr;
		void (*post_message)(void *context)                                              = nullptr;
	};

	struct Descriptors {
		int tty_fd         = -1;
		int abort_read_fd  = -1;
		int abort_write_fd = -1;
	};

	enum class PromptIoResult : std::uint8_t {
		retry,
		ready,
		abort,
	};

	NativePromptConversation(pam_handle_t *pamh, struct pam_conv original_conv,
	                         bool has_original_conv, Descriptors descriptors,
	                         Operations operations);
	static auto production_operations() -> Operations;

	static auto dispatch(int num_msg, const struct pam_message **msgm,
	                     struct pam_response **response, void *appdata_ptr) -> int;
	auto        handle(int num_msg, const struct pam_message **msgm, struct pam_response **response)
	    -> int;
	[[nodiscard]] auto write_message_line(const struct pam_message &message) const -> int;
	auto prompt_input(const struct pam_message &message, char **response, bool hide_input) -> int;
	auto poll_prompt(std::array<struct pollfd, 2> &fds) const -> int;
	auto read_prompt_char(char *ch) const -> ssize_t;
	auto poll_prompt_state(std::array<struct pollfd, 2> &fds) -> PromptIoResult;
	auto read_prompt_state(char *ch) -> PromptIoResult;
	auto wait_for_prompt_character(char *ch) -> PromptIoResult;
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
	Operations         operations_{};
};

#endif  // HOWDY_PAM_NATIVE_PROMPT_CONVERSATION_HPP
