#ifndef HOWDY_PAM_NATIVE_PROMPT_CONVERSATION_HPP
#define HOWDY_PAM_NATIVE_PROMPT_CONVERSATION_HPP

#include "prompt/conversation_restore.hpp"
#include "support/scoped_fd.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <poll.h>
#include <termios.h>

#include <security/pam_appl.h>

#include <sys/types.h>

class NativePromptConversationTestAccess;

class NativePrompt {
public:
	virtual ~NativePrompt() = default;

	[[nodiscard]] virtual auto Available() const -> bool                             = 0;
	virtual auto               Install() -> int                                      = 0;
	virtual void               RequestAbort()                                        = 0;
	[[nodiscard]] virtual auto TerminalRestoreFailed() const noexcept -> bool        = 0;
	virtual auto RestoreOriginal() noexcept -> howdy::pam::ConversationRestoreResult = 0;
};

class NativePromptConversation final : public NativePrompt {
public:
	explicit NativePromptConversation(pam_handle_t *pamh);
	~NativePromptConversation() override;

	NativePromptConversation(const NativePromptConversation &)                     = delete;
	auto operator=(const NativePromptConversation &) -> NativePromptConversation & = delete;

	[[nodiscard]] auto Available() const -> bool override;
	auto               Install() -> int override;
	void               RequestAbort() override;
	[[nodiscard]] auto TerminalRestoreFailed() const noexcept -> bool override;
	auto               RestoreOriginal() noexcept -> howdy::pam::ConversationRestoreResult override;

private:
	friend class NativePromptConversationTestAccess;

	struct Operations {
		void *context                                                                    = nullptr;
		int (*poll_prompt)(void *context, struct pollfd *fds, nfds_t count, int timeout) = nullptr;
		ssize_t (*read_prompt)(void *context, int fd, void *buffer, std::size_t count)   = nullptr;
		int (*restore_terminal)(void *context, int fd, const struct termios *termios)    = nullptr;
		void (*post_message)(void *context)                                              = nullptr;
		int (*set_pam_item)(void *context, pam_handle_t *pamh, int item_type,
		                    const void *item)                                            = nullptr;
	};

	struct DispatchContext {
		std::atomic<bool>         fail_closed{false};
		NativePromptConversation *owner = nullptr;
	};

	struct Descriptors {
		howdy::native::ScopedFd tty_fd;
		howdy::native::ScopedFd abort_read_fd;
		howdy::native::ScopedFd abort_write_fd;
	};

	enum class PromptIoResult : std::uint8_t {
		kRetry,
		kReady,
		kAbort,
	};

	NativePromptConversation(pam_handle_t *pamh, struct pam_conv original_conv,
	                         bool has_original_conv, Descriptors descriptors,
	                         Operations operations);
	static auto ProductionOperations() -> Operations;

	static auto Dispatch(int num_msg, const struct pam_message **msgm,
	                     struct pam_response **response, void *appdata_ptr) -> int;
	auto        Handle(int num_msg, const struct pam_message **msgm, struct pam_response **response)
	    -> int;
	[[nodiscard]] auto WriteMessageLine(const struct pam_message &message) const -> int;
	auto PromptInput(const struct pam_message &message, char **response, bool hide_input) -> int;
	auto PollPrompt(std::array<struct pollfd, 2> &fds) const -> int;
	auto ReadPromptChar(char *ch) const -> ssize_t;
	auto PollPromptState(std::array<struct pollfd, 2> &fds) -> PromptIoResult;
	auto ReadPromptState(char *ch) -> PromptIoResult;
	auto WaitForPromptCharacter(char *ch) -> PromptIoResult;
	[[nodiscard]] auto RestorePromptTerminal(const struct termios &original_termios) const -> bool;
	void               RetainUnsafeDispatchContext() noexcept;

	pam_handle_t                          *pamh_ = nullptr;
	struct pam_conv                        original_conv_{};
	std::unique_ptr<DispatchContext>       dispatch_context_;
	struct pam_conv                        override_conv_{};
	bool                                   has_original_conv_ = false;
	bool                                   installed_         = false;
	howdy::native::ScopedFd                tty_fd_;
	std::array<howdy::native::ScopedFd, 2> abort_pipe_;
	std::atomic<bool>                      abort_requested_{false};
	std::atomic<bool>                      terminal_restore_failed_{false};
	Operations                             operations_{};
};

struct NativeTerminalDescriptors {
	int tty    = -1;
	int input  = -1;
	int output = -1;
	int error  = -1;
};

__attribute__((visibility("hidden"))) auto
NativePromptTerminalIsInteractive(const NativeTerminalDescriptors &descriptors) -> bool;

#endif  // HOWDY_PAM_NATIVE_PROMPT_CONVERSATION_HPP
