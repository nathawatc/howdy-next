#include "prompt/native_prompt_conversation.hpp"

#include "conversation_restore/internal.hpp"
#include "prompt/conversation_response.hpp"
#include "prompt/conversation_restore.hpp"
#include "prompt/internal_fd.hpp"
#include "prompt/native_prompt_input.hpp"
#include "support/bit_flags.hpp"
#include "support/eintr.hpp"
#include "support/fd_io.hpp"
#include "support/scoped_fd.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#include <security/pam_ext.h>

#include <sys/ioctl.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <sys/syslog.h>

namespace {

	constexpr int kAbortPollTimeoutMs = 100;

	auto ProductionPoll(void * /*context*/, struct pollfd *fds, nfds_t count, int timeout) -> int {
		return poll(fds, count, timeout);
	}

	auto ProductionRead(void * /*context*/, int fd, void *buffer, std::size_t count) -> ssize_t {
		return read(fd, buffer, count);
	}

	auto ProductionRestoreTerminal(void * /*context*/, int fd, const struct termios *termios)
	    -> int {
		return tcsetattr(fd, TCSANOW, termios);
	}

	void ProductionPostMessage(void * /*context*/) {}

	auto ProductionSetPamItem(void * /*context*/, pam_handle_t *pamh, int item_type,
	                          const void *item) -> int {
		return pam_set_item(pamh, item_type, item);
	}

	auto NativePromptFailClosedDispatch(int /*num_msg*/, const struct pam_message ** /*msgm*/,
	                                    struct pam_response **response, void * /*appdata_ptr*/)
	    -> int {
		if (response != nullptr) {
			*response = nullptr;
		}
		return PAM_CONV_ERR;
	}

	auto OpenTtyFd(pam_handle_t *pamh) -> howdy::native::ScopedFd {
		const void *tty_item = nullptr;
		if (pam_get_item(pamh, PAM_TTY, &tty_item) != PAM_SUCCESS || tty_item == nullptr) {
			return {};
		}

		auto tty_path = std::string(static_cast<const char *>(tty_item));
		if (tty_path.empty()) {
			return {};
		}
		if (tty_path.front() != '/') {
			tty_path = "/dev/" + tty_path;
		}

		auto tty_fd = howdy::pam::detail::NormalizeInternalFd(
		    howdy::pam::detail::ScopedFd(open(tty_path.c_str(), O_RDWR | O_CLOEXEC | O_NOCTTY)));
		if (!tty_fd.Valid()) {
			return {};
		}

		if (!NativePromptTerminalIsInteractive({
		        .tty    = tty_fd.Get(),
		        .input  = STDIN_FILENO,
		        .output = STDOUT_FILENO,
		        .error  = STDERR_FILENO,
		    })) {
			return {};
		}
		return tty_fd;
	}

	auto WriteAll(int fd, const std::string &text) -> bool {
		return howdy::native::WriteAllToFd(fd, text);
	}

	auto WriteNewline(int fd) -> void {
		(void)howdy::native::WriteAllToFd(fd, "\n");
	}

	void DrainAbortPipe(int fd) {
		std::array<char, 32> buffer{};
		while (read(fd, buffer.data(), buffer.size()) > 0) {
		}
	}

	auto SameTerminalIdentity([[maybe_unused]] int target_fd, const struct stat &target_stat,
	                          int descriptor_fd) -> bool {
		struct stat descriptor_stat{};
		if (fstat(descriptor_fd, &descriptor_stat) != 0 || !S_ISCHR(descriptor_stat.st_mode)) {
			return false;
		}
		if (descriptor_stat.st_rdev == target_stat.st_rdev) {
			return true;
		}

#ifdef TIOCGDEV
		unsigned int target_device     = 0;
		unsigned int descriptor_device = 0;
		if (ioctl(target_fd, TIOCGDEV, &target_device) == 0 &&
		    ioctl(descriptor_fd, TIOCGDEV, &descriptor_device) == 0) {
			return target_device == descriptor_device;
		}
#endif
		return false;
	}

}  // namespace

auto NativePromptTerminalIsInteractive(const NativeTerminalDescriptors &descriptors) -> bool {
	if (descriptors.tty < 0 || isatty(descriptors.tty) == 0) {
		return false;
	}

	struct stat tty_stat{};
	if (fstat(descriptors.tty, &tty_stat) != 0 || !S_ISCHR(tty_stat.st_mode)) {
		return false;
	}

	const pid_t foreground_group = tcgetpgrp(descriptors.tty);
	if (foreground_group < 0 || foreground_group != getpgrp()) {
		return false;
	}

	// Native prompt owns PAM_TTY for input and output. Require at least one standard
	// descriptor to identify that same foreground terminal; any others may be redirected.
	return std::ranges::any_of(std::array{descriptors.input, descriptors.output, descriptors.error},
	                           [descriptors, &tty_stat](const int fd) -> bool {
		                           return fd >= 0 && isatty(fd) != 0 &&
								          SameTerminalIdentity(descriptors.tty, tty_stat, fd);
	                           });
}

NativePromptConversation::NativePromptConversation(pam_handle_t *pamh)
    : pamh_(pamh)
    , dispatch_context_(std::make_unique<DispatchContext>())
    , override_conv_{.conv = Dispatch, .appdata_ptr = dispatch_context_.get()}
    , operations_(ProductionOperations()) {
	dispatch_context_->owner = this;
	const void *conv_ptr     = nullptr;
	if (pam_get_item(pamh_, PAM_CONV, &conv_ptr) != PAM_SUCCESS || conv_ptr == nullptr) {
		return;
	}

	original_conv_     = *static_cast<const struct pam_conv *>(conv_ptr);
	has_original_conv_ = original_conv_.conv != nullptr;
	if (!has_original_conv_) {
		return;
	}

	tty_fd_ = OpenTtyFd(pamh_);
	if (!tty_fd_.Valid()) {
		return;
	}

	auto abort_pipe = howdy::pam::detail::CreateInternalPipe(O_CLOEXEC | O_NONBLOCK);
	if (!howdy::pam::detail::InternalPipeValid(abort_pipe)) {
		tty_fd_.Reset();
		return;
	}
	abort_pipe_ = std::move(abort_pipe);
}

auto NativePromptConversation::ProductionOperations() -> Operations {
	return {
	    .poll_prompt      = ProductionPoll,
	    .read_prompt      = ProductionRead,
	    .restore_terminal = ProductionRestoreTerminal,
	    .post_message     = ProductionPostMessage,
	    .set_pam_item     = ProductionSetPamItem,
	};
}

NativePromptConversation::NativePromptConversation(pam_handle_t   *pamh,
                                                   struct pam_conv original_conv,
                                                   bool has_original_conv, Descriptors descriptors,
                                                   Operations operations)
    : pamh_(pamh)
    , original_conv_(original_conv)
    , dispatch_context_(std::make_unique<DispatchContext>())
    , override_conv_{.conv = Dispatch, .appdata_ptr = dispatch_context_.get()}
    , has_original_conv_(has_original_conv)
    , tty_fd_(std::move(descriptors.tty_fd))
    , abort_pipe_{.read  = std::move(descriptors.abort_read_fd),
	              .write = std::move(descriptors.abort_write_fd),}
    , operations_(operations) {
	dispatch_context_->owner = this;
}

NativePromptConversation::~NativePromptConversation() {
	if (installed_ && dispatch_context_ != nullptr) {
		RetainUnsafeDispatchContext();
	}
}

void NativePromptConversation::RetainUnsafeDispatchContext() noexcept {
	dispatch_context_->fail_closed.store(true);
	dispatch_context_->owner = nullptr;
	if (!howdy::pam::QuarantineUnsafeCallbackContext(dispatch_context_)) {
		pam_syslog(pamh_, LOG_CRIT,
		           "Native fail-closed callback context retained outside quarantine");
	}
}

auto NativePromptConversation::RestoreOriginal() noexcept -> howdy::pam::ConversationRestoreResult {
	if (!installed_) {
		return howdy::pam::ConversationRestoreResult::kOriginalRestored;
	}

	if (pamh_ == nullptr) {
		pam_syslog(pamh_, LOG_CRIT, "Cannot restore PAM conversation: null PAM handle");
		RetainUnsafeDispatchContext();
		installed_ = false;
		return howdy::pam::ConversationRestoreResult::kUnsafe;
	}

	const int restore_result =
	    operations_.set_pam_item(operations_.context, pamh_, PAM_CONV, &original_conv_);
	if (restore_result == PAM_SUCCESS) {
		installed_ = false;
		return howdy::pam::ConversationRestoreResult::kOriginalRestored;
	}

	pam_syslog(pamh_, LOG_CRIT, "Failed to restore original PAM conversation: %d", restore_result);
	static const struct pam_conv kFailClosedConv = {
	    .conv        = NativePromptFailClosedDispatch,
	    .appdata_ptr = nullptr,
	};
	const int fail_closed_result =
	    operations_.set_pam_item(operations_.context, pamh_, PAM_CONV, &kFailClosedConv);
	if (fail_closed_result != PAM_SUCCESS) {
		pam_syslog(pamh_, LOG_CRIT, "Failed to install fail-closed PAM conversation: %d",
		           fail_closed_result);
		RetainUnsafeDispatchContext();
		installed_ = false;
		return howdy::pam::ConversationRestoreResult::kUnsafe;
	}
	installed_ = false;
	return howdy::pam::ConversationRestoreResult::kFailClosedInstalled;
}

auto NativePromptConversation::Available() const -> bool {
	return has_original_conv_ && tty_fd_.Valid() && abort_pipe_.read.Valid() &&
	       abort_pipe_.write.Valid();
}

auto NativePromptConversation::Install() -> int {
	if (!Available()) {
		return PAM_SYSTEM_ERR;
	}

	const int pam_res =
	    operations_.set_pam_item(operations_.context, pamh_, PAM_CONV, &override_conv_);
	if (pam_res == PAM_SUCCESS) {
		installed_ = true;
	}
	return pam_res;
}

void NativePromptConversation::RequestAbort() {
	abort_requested_.store(true);
	if (!abort_pipe_.write.Valid()) {
		return;
	}

	constexpr char signal = 'x';
	(void)howdy::native::RetryOnEintr([&] -> ssize_t {
		return write(abort_pipe_.write.Get(), &signal, 1);
	});
}

[[nodiscard]] auto NativePromptConversation::TerminalRestoreFailed() const noexcept -> bool {
	return terminal_restore_failed_.load();
}

auto NativePromptConversation::Dispatch(int num_msg, const struct pam_message **msgm,
                                        struct pam_response **response, void *appdata_ptr) -> int {
	if (response != nullptr) {
		*response = nullptr;
	}

	try {
		auto const *context = static_cast<DispatchContext *>(appdata_ptr);
		if (context == nullptr || response == nullptr || context->fail_closed.load() ||
		    context->owner == nullptr) {
			return PAM_CONV_ERR;
		}
		return context->owner->Handle(num_msg, msgm, response);
	} catch (const std::exception &error) {
		pam_syslog(nullptr, LOG_ERR, "Unhandled C++ exception in native PAM conversation: %s",
		           error.what());
		if (response != nullptr) {
			howdy::pam::SecureFreeConversationResponses(response, num_msg);
		}
		return PAM_CONV_ERR;
	} catch (...) {
		pam_syslog(nullptr, LOG_ERR, "Unhandled non-standard exception in native PAM conversation");
		if (response != nullptr) {
			howdy::pam::SecureFreeConversationResponses(response, num_msg);
		}
		return PAM_CONV_ERR;
	}
}

auto NativePromptConversation::Handle(int num_msg, const struct pam_message **msgm,
                                      struct pam_response **response) -> int {
	if (num_msg <= 0 || msgm == nullptr || response == nullptr) {
		return PAM_CONV_ERR;
	}

	const auto               count = static_cast<std::size_t>(num_msg);
	std::vector<std::size_t> response_lengths(count, 0);
	auto                    *pam_responses =
	    static_cast<struct pam_response *>(std::calloc(count, sizeof(struct pam_response)));
	if (pam_responses == nullptr) {
		return PAM_BUF_ERR;
	}

	class ResponseCleanupGuard {
	public:
		ResponseCleanupGuard(struct pam_response     **responses,
		                     std::vector<std::size_t> *lengths) noexcept
		    : responses_(responses)
		    , lengths_(lengths) {}

		ResponseCleanupGuard(const ResponseCleanupGuard &)                     = delete;
		auto operator=(const ResponseCleanupGuard &) -> ResponseCleanupGuard & = delete;
		ResponseCleanupGuard(ResponseCleanupGuard &&)                          = delete;
		auto operator=(ResponseCleanupGuard &&) -> ResponseCleanupGuard &      = delete;

		void Release() noexcept {
			responses_ = nullptr;
		}

		~ResponseCleanupGuard() {
			if (responses_ != nullptr && *responses_ != nullptr && lengths_ != nullptr) {
				howdy::pam::SecureFreeConversationResponses(responses_, *lengths_);
			}
		}

	private:
		struct pam_response     **responses_ = nullptr;
		std::vector<std::size_t> *lengths_   = nullptr;
	} cleanup_guard{&pam_responses, &response_lengths};

	const auto messages    = std::span(msgm, count);
	auto const responses   = std::span(pam_responses, count);
	auto       response_it = responses.begin();
	auto       length_it   = response_lengths.begin();
	for (const auto *message_ptr : messages) {
		if (message_ptr == nullptr) {
			return PAM_CONV_ERR;
		}

		auto                     &current_response = *response_it++;
		auto                     &current_length   = *length_it++;
		const struct pam_message &message          = *message_ptr;
		int                       result           = PAM_SUCCESS;
		switch (message.msg_style) {
			case PAM_PROMPT_ECHO_OFF:
				result = PromptInput(message, &current_response.resp, true, &current_length);
				break;
			case PAM_PROMPT_ECHO_ON:
				result = PromptInput(message, &current_response.resp, false, &current_length);
				break;
			case PAM_TEXT_INFO:
			case PAM_ERROR_MSG:
				result = WriteMessageLine(message);
				break;
			default:
				result = PAM_CONV_ERR;
				break;
		}

		if (operations_.post_message != nullptr) {
			operations_.post_message(operations_.context);
		}

		if (result == PAM_SUCCESS) {
			continue;
		}

		return result;
	}

	*response = pam_responses;
	cleanup_guard.Release();
	return PAM_SUCCESS;
}

auto NativePromptConversation::WriteMessageLine(const struct pam_message &message) const -> int {
	if (!tty_fd_.Valid()) {
		return PAM_CONV_ERR;
	}

	const std::string text = message.msg == nullptr ? "" : message.msg;
	if (!WriteAll(tty_fd_.Get(), text)) {
		return PAM_CONV_ERR;
	}

	WriteNewline(tty_fd_.Get());
	return PAM_SUCCESS;
}

auto NativePromptConversation::RestorePromptTerminal(const struct termios &original_termios) const
    -> bool {
	return howdy::native::RetryOnEintr([&] -> int {
		       return operations_.restore_terminal(operations_.context, tty_fd_.Get(),
			                                       &original_termios);
	       }) == 0;
}

auto NativePromptConversation::PollPrompt(std::array<struct pollfd, 2> &fds) const -> int {
	return operations_.poll_prompt(operations_.context, fds.data(), fds.size(),
	                               kAbortPollTimeoutMs);
}

auto NativePromptConversation::ReadPromptChar(char *ch) const -> ssize_t {
	return operations_.read_prompt(operations_.context, tty_fd_.Get(), ch, 1);
}

auto NativePromptConversation::PollPromptState(std::array<struct pollfd, 2> &fds)
    -> PromptIoResult {
	const int poll_result = PollPrompt(fds);
	if (poll_result < 0) {
		return errno == EINTR && !abort_requested_.load() ? PromptIoResult::kRetry
		                                                  : PromptIoResult::kAbort;
	}
	if (abort_requested_.load()) {
		DrainAbortPipe(abort_pipe_.read.Get());
		return PromptIoResult::kAbort;
	}
	const auto    &tty_poll          = fds.front();
	const auto    &abort_poll        = fds.back();
	constexpr auto fd_failure_events = howdy::native::CombineBits(POLLHUP, POLLERR, POLLNVAL);
	if (howdy::native::HasAnyBits(tty_poll.revents, fd_failure_events) ||
	    howdy::native::HasAnyBits(abort_poll.revents, fd_failure_events)) {
		return PromptIoResult::kAbort;
	}
	if (howdy::native::HasAnyBits(abort_poll.revents, POLLIN)) {
		DrainAbortPipe(abort_pipe_.read.Get());
		return PromptIoResult::kAbort;
	}
	return poll_result == 0 || !howdy::native::HasAnyBits(tty_poll.revents, POLLIN)
	           ? PromptIoResult::kRetry
			   : PromptIoResult::kReady;
}

auto NativePromptConversation::ReadPromptState(char *ch) -> PromptIoResult {
	const ssize_t bytes_read = ReadPromptChar(ch);
	if (bytes_read < 0) {
		return errno == EINTR && !abort_requested_.load() ? PromptIoResult::kRetry
		                                                  : PromptIoResult::kAbort;
	}
	return bytes_read == 0 ? PromptIoResult::kRetry : PromptIoResult::kReady;
}

auto NativePromptConversation::WaitForPromptCharacter(char *ch) -> PromptIoResult {
	std::array<struct pollfd, 2> fds{
	    {
	        {.fd = tty_fd_.Get(), .events = POLLIN, .revents = 0},
	        {.fd = abort_pipe_.read.Get(), .events = POLLIN, .revents = 0},
	    },
	};
	while (true) {
		const auto poll_result = PollPromptState(fds);
		if (poll_result == PromptIoResult::kAbort) {
			return poll_result;
		}
		if (poll_result == PromptIoResult::kRetry) {
			continue;
		}
		const auto read_result = ReadPromptState(ch);
		if (read_result != PromptIoResult::kRetry) {
			return read_result;
		}
	}
}

auto NativePromptConversation::ReadPromptPassword(
    howdy::pam::native_prompt_input::SensitiveBuffer &password, bool &response_too_long) -> bool {
	while (true) {
		char ch = '\0';
		if (WaitForPromptCharacter(&ch) == PromptIoResult::kAbort) {
			return false;
		}

		const auto character_result =
		    howdy::pam::native_prompt_input::ProcessCharacter(ch, password, response_too_long);
		if (character_result == howdy::pam::native_prompt_input::CharacterResult::kComplete) {
			return true;
		}
		if (character_result == howdy::pam::native_prompt_input::CharacterResult::kAbort) {
			return false;
		}
	}
}

auto NativePromptConversation::PromptInput(const struct pam_message &message, char **response,
                                           bool hide_input, std::size_t *response_length) -> int {
	if (response == nullptr || !tty_fd_.Valid() || abort_requested_.load()) {
		return PAM_CONV_ERR;
	}
	if (response_length != nullptr) {
		*response_length = 0;
	}

	struct termios original_termios{};
	if (tcgetattr(tty_fd_.Get(), &original_termios) != 0) {
		return PAM_CONV_ERR;
	}

	struct termios prompt_termios = original_termios;
	prompt_termios.c_lflag &= static_cast<tcflag_t>(~ICANON);
	prompt_termios.c_lflag &= static_cast<tcflag_t>(~ISIG);
	if (hide_input) {
		prompt_termios.c_lflag &= static_cast<tcflag_t>(~ECHO);
	} else {
		prompt_termios.c_lflag |= ECHO;
	}
	prompt_termios.c_cc[VMIN]  = 1;
	prompt_termios.c_cc[VTIME] = 0;
	if (tcsetattr(tty_fd_.Get(), TCSANOW, &prompt_termios) != 0) {
		return PAM_CONV_ERR;
	}

	class PromptTerminalGuard {
	public:
		PromptTerminalGuard(NativePromptConversation *owner,
		                    const struct termios     *original) noexcept
		    : owner_(owner)
		    , original_(original) {}

		PromptTerminalGuard(const PromptTerminalGuard &)                     = delete;
		auto operator=(const PromptTerminalGuard &) -> PromptTerminalGuard & = delete;
		PromptTerminalGuard(PromptTerminalGuard &&)                          = delete;
		auto operator=(PromptTerminalGuard &&) -> PromptTerminalGuard &      = delete;

		[[nodiscard]] auto Restore() -> bool {
			if (owner_ == nullptr) {
				return true;
			}
			auto      *owner    = std::exchange(owner_, nullptr);
			const bool restored = owner->RestorePromptTerminal(*original_);
			if (!restored) {
				owner->terminal_restore_failed_.store(true);
			}
			WriteNewline(owner->tty_fd_.Get());
			return restored;
		}

		~PromptTerminalGuard() {
			(void)Restore();
		}

	private:
		NativePromptConversation *owner_    = nullptr;
		const struct termios     *original_ = nullptr;
	} terminal_guard{this, &original_termios};

	const std::string prompt_text = message.msg == nullptr ? "" : message.msg;
	if (!WriteAll(tty_fd_.Get(), prompt_text)) {
		return PAM_CONV_ERR;
	}

	howdy::pam::native_prompt_input::SensitiveBuffer password;
	bool                                             response_too_long = false;
	if (!ReadPromptPassword(password, response_too_long) || !terminal_guard.Restore() ||
	    response_too_long) {
		return PAM_CONV_ERR;
	}

	char *response_buffer = static_cast<char *>(std::calloc(password.Size() + 1, sizeof(char)));
	if (response_buffer == nullptr) {
		return PAM_BUF_ERR;
	}

	std::memcpy(response_buffer, password.Data(), password.Size());
	*response = response_buffer;
	if (response_length != nullptr) {
		*response_length = password.Size() + 1;
	}
	return PAM_SUCCESS;
}
