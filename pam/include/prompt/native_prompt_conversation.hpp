#pragma once

#include "prompt/conversation_restore.hpp"
#include "prompt/internal_fd.hpp"
#include "prompt/native_prompt_input.hpp"
#include "support/scoped_fd.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <poll.h>
#include <termios.h>

#include <security/pam_appl.h>

#include <sys/types.h>

class NativePromptConversationTestAccess;

class NativePrompt {
public:
	NativePrompt()                                         = default;
	NativePrompt(const NativePrompt &)                     = delete;
	auto operator=(const NativePrompt &) -> NativePrompt & = delete;
	NativePrompt(NativePrompt &&)                          = delete;
	auto operator=(NativePrompt &&) -> NativePrompt &      = delete;
	virtual ~NativePrompt()                                = default;

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
	NativePromptConversation(NativePromptConversation &&)                          = delete;
	auto operator=(NativePromptConversation &&) -> NativePromptConversation &      = delete;

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
	auto PromptInput(const struct pam_message &message, char **response, bool hide_input,
	                 std::size_t *response_length = nullptr) -> int;
	auto ReadPromptPassword(const struct termios                             &terminal,
	                        howdy::pam::native_prompt_input::SensitiveBuffer &password,
	                        bool &response_too_long) -> bool;
	auto PollPrompt(std::array<struct pollfd, 2> &fds) const -> int;
	[[nodiscard]] auto ReadPromptChunk(std::span<char> buffer) const -> ssize_t;
	auto               PollPromptState(std::array<struct pollfd, 2> &fds) -> PromptIoResult;
	auto ReadPromptState(std::span<char> buffer, std::size_t *count) -> PromptIoResult;
	auto WaitForPromptInput(std::span<char> buffer, std::size_t *count) -> PromptIoResult;
	[[nodiscard]] auto RestorePromptTerminal(const struct termios &original_termios) const -> bool;
	void               RetainUnsafeDispatchContext() noexcept;

	pam_handle_t                    *pamh_ = nullptr;
	struct pam_conv                  original_conv_{};
	std::unique_ptr<DispatchContext> dispatch_context_;
	struct pam_conv                  override_conv_{};
	bool                             has_original_conv_ = false;
	bool                             installed_         = false;
	howdy::native::ScopedFd          tty_fd_;
	howdy::pam::detail::InternalPipe abort_pipe_;
	std::atomic<bool>                abort_requested_{false};
	std::atomic<bool>                terminal_restore_failed_{false};
	Operations                       operations_{};
};

struct NativeTerminalDescriptors {
	int tty    = -1;
	int input  = -1;
	int output = -1;
	int error  = -1;
};

struct NativePromptTtyTarget {
	std::string path;
	dev_t       device  = 0;
	ino_t       inode   = 0;
	dev_t       rdevice = 0;
};

// Canonicalizes a PAM_TTY value. Accepts only a character device whose canonical path is under
// /dev/; the returned identity lets the caller verify that the opened descriptor is that node.
__attribute__((visibility("hidden"))) auto ResolveNativePromptTty(std::string_view pam_tty)
    -> std::optional<NativePromptTtyTarget>;

__attribute__((visibility("hidden"))) auto
NativePromptTerminalIsInteractive(const NativeTerminalDescriptors &descriptors) -> bool;
