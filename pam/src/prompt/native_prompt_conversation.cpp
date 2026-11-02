#include "prompt/native_prompt_conversation.hpp"

#include "conversation_restore/internal.hpp"
#include "prompt/conversation_response.hpp"
#include "prompt/conversation_restore.hpp"
#include "prompt/internal_fd.hpp"
#include "prompt/native_prompt_input.hpp"
#include "support/bit_flags.hpp"
#include "support/eintr.hpp"
#include "support/scoped_fd.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
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

		const auto target = ResolveNativePromptTty(static_cast<const char *>(tty_item));
		if (!target.has_value()) {
			return {};
		}

		auto tty_fd = howdy::pam::detail::NormalizeInternalFd(howdy::pam::detail::ScopedFd(
		    open(target->path.c_str(), O_RDWR | O_CLOEXEC | O_NOCTTY | O_NONBLOCK | O_NOFOLLOW)));
		if (!tty_fd.Valid()) {
			return {};
		}

		// O_NOFOLLOW covers only the last component; confirm the descriptor is the approved
		// character device so a swapped parent directory cannot redirect the open. Not covered by
		// tests: forcing a mismatch would need a race seam between resolve and open.
		struct stat opened{};
		if (fstat(tty_fd.Get(), &opened) != 0 || !S_ISCHR(opened.st_mode) ||
		    opened.st_dev != target->device || opened.st_ino != target->inode ||
		    opened.st_rdev != target->rdevice) {
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

	// The tty is O_NONBLOCK (so a stolen byte cannot park read() past an abort); wait for output
	// room instead of failing the prompt on EAGAIN, but give up once an abort is requested (^S
	// can stop output indefinitely).
	auto WriteAll(int fd, std::string_view text, const std::atomic<bool> &abort_requested) -> bool {
		while (!text.empty()) {
			const ssize_t written = howdy::native::RetryOnEintr([&] -> ssize_t {
				return write(fd, text.data(), text.size());
			});
			if (written > 0) {
				text.remove_prefix(static_cast<std::size_t>(written));
				continue;
			}
			if (written == 0 || (errno != EAGAIN && errno != EWOULDBLOCK) ||
			    abort_requested.load()) {
				return false;
			}
			struct pollfd out{.fd = fd, .events = POLLOUT, .revents = 0};
			if (howdy::native::RetryOnEintr([&] -> int {
				    return poll(&out, 1, kAbortPollTimeoutMs);
			    }) < 0) {
				return false;
			}
		}
		return true;
	}

	auto PromptTermios(const termios &original_termios, bool hide_input) -> termios {
		// Canonical mode leaves line editing (erase, kill, word erase, EOF, IUTF8) to the kernel
		// with the user's stty characters, as getpass-style prompts do. ICRNL, IEXTEN and
		// OPOST/ONLCR are forced so a raw-mode caller gets the same Enter, editing and output.
		// ISIG stays off so ^C cannot kill the caller with the terminal unrestored; VEOL/VEOL2 end
		// the line on ^C and ^Z so ProcessCharacter aborts at once. Literal next is off, so every
		// '\n' ends a line and no signal character can hide in the password.
		struct termios prompt_termios = original_termios;
		prompt_termios.c_iflag |= ICRNL;
		prompt_termios.c_iflag &= static_cast<tcflag_t>(~INLCR);
		prompt_termios.c_oflag |= OPOST;
		prompt_termios.c_oflag |= ONLCR;
		prompt_termios.c_lflag |= ICANON;
		prompt_termios.c_lflag |= IEXTEN;
		prompt_termios.c_lflag &= static_cast<tcflag_t>(~ISIG);
		prompt_termios.c_cc[VLNEXT] = _POSIX_VDISABLE;
		// shortcut: ^\ has no slot left and aborts only on Enter, and a user-set eol/eol2 is
		// replaced while the prompt is shown; fine while both are rare.
		prompt_termios.c_cc[VEOL]  = original_termios.c_cc[VINTR];
		prompt_termios.c_cc[VEOL2] = original_termios.c_cc[VSUSP];
		if (hide_input) {
			prompt_termios.c_lflag &= static_cast<tcflag_t>(~ECHO);
			prompt_termios.c_lflag &= static_cast<tcflag_t>(~ECHONL);
		} else {
			prompt_termios.c_lflag |= ECHO;
		}
		return prompt_termios;
	}

	// One read() into this holds a whole line: n_tty caps a canonical line at N_TTY_BUF_SIZE.
	// Erased on every exit path, exceptions included.
	class TtyLine {
	public:
		TtyLine()                                    = default;
		TtyLine(const TtyLine &)                     = delete;
		auto operator=(const TtyLine &) -> TtyLine & = delete;
		TtyLine(TtyLine &&)                          = delete;
		auto operator=(TtyLine &&) -> TtyLine &      = delete;

		~TtyLine() {
			explicit_bzero(bytes_.data(), bytes_.size());
		}

		[[nodiscard]] auto Bytes() -> std::span<char> {
			return bytes_;
		}

	private:
		std::array<char, 4096> bytes_{};
	};

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

auto ResolveNativePromptTty(std::string_view pam_tty) -> std::optional<NativePromptTtyTarget> {
	if (pam_tty.empty()) {
		return std::nullopt;
	}
	auto path = std::string(pam_tty);
	if (path.front() != '/') {
		path = "/dev/" + path;
	}

	// Resolve symlinks and ".." so PAM_TTY cannot steer the open outside /dev.
	const std::unique_ptr<char, decltype(&std::free)> canonical(realpath(path.c_str(), nullptr),
	                                                            &std::free);
	if (canonical == nullptr || !std::string_view(canonical.get()).starts_with("/dev/")) {
		return std::nullopt;
	}

	// /dev also holds world-writable directories (shm, mqueue); never open anything but a
	// character device.
	struct stat metadata{};
	if (stat(canonical.get(), &metadata) != 0 || !S_ISCHR(metadata.st_mode)) {
		return std::nullopt;
	}
	return NativePromptTtyTarget{
	    .path    = canonical.get(),
	    .device  = metadata.st_dev,
	    .inode   = metadata.st_ino,
	    .rdevice = metadata.st_rdev,
	};
}

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

	std::string text = message.msg == nullptr ? "" : message.msg;
	text += '\n';
	if (!WriteAll(tty_fd_.Get(), text, abort_requested_)) {
		return PAM_CONV_ERR;
	}
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

auto NativePromptConversation::ReadPromptChunk(std::span<char> buffer) const -> ssize_t {
	return operations_.read_prompt(operations_.context, tty_fd_.Get(), buffer.data(),
	                               buffer.size());
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

auto NativePromptConversation::ReadPromptState(std::span<char> buffer, std::size_t *count)
    -> PromptIoResult {
	const ssize_t bytes_read = ReadPromptChunk(buffer);
	if (bytes_read < 0) {
		// EAGAIN: another reader took the line after poll(); go back to poll() so abort is seen.
		const bool transient = errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK;
		return transient && !abort_requested_.load() ? PromptIoResult::kRetry
		                                             : PromptIoResult::kAbort;
	}
	// Canonical mode: 0 is EOF (^D on an empty line) or hangup, as for getpass().
	if (bytes_read == 0) {
		return PromptIoResult::kAbort;
	}
	*count = static_cast<std::size_t>(bytes_read);
	return PromptIoResult::kReady;
}

auto NativePromptConversation::WaitForPromptInput(std::span<char> buffer, std::size_t *count)
    -> PromptIoResult {
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
		const auto read_result = ReadPromptState(buffer, count);
		if (read_result != PromptIoResult::kRetry) {
			return read_result;
		}
	}
}

auto NativePromptConversation::ReadPromptPassword(
    const struct termios &terminal, howdy::pam::native_prompt_input::SensitiveBuffer &password,
    bool &response_too_long) -> bool {
	// One read() takes the whole line; reading byte by byte would see a mid-line EOF marker as
	// end of file on pre-5.12 kernels.
	TtyLine line;
	while (true) {
		std::size_t count = 0;
		if (WaitForPromptInput(line.Bytes(), &count) == PromptIoResult::kAbort) {
			return false;
		}

		// shortcut: a raw-mode caller's typeahead arrives as one pushed line; bytes after a '\r'
		// in it are discarded and an echo-on prompt then lacks its newline. Rare; fail-safe.
		for (const char ch : line.Bytes().first(count)) {
			const auto character_result = howdy::pam::native_prompt_input::ProcessCharacter(
			    ch, terminal, password, response_too_long);
			if (character_result == howdy::pam::native_prompt_input::CharacterResult::kComplete) {
				return true;
			}
			if (character_result == howdy::pam::native_prompt_input::CharacterResult::kAbort) {
				return false;
			}
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

	const struct termios prompt_termios = PromptTermios(original_termios, hide_input);
	if (tcsetattr(tty_fd_.Get(), TCSANOW, &prompt_termios) != 0) {
		return PAM_CONV_ERR;
	}

	class PromptTerminalGuard {
	public:
		PromptTerminalGuard(NativePromptConversation *owner, const struct termios *original,
		                    bool echo) noexcept
		    : owner_(owner)
		    , original_(original)
		    , echo_(echo) {}

		PromptTerminalGuard(const PromptTerminalGuard &)                     = delete;
		auto operator=(const PromptTerminalGuard &) -> PromptTerminalGuard & = delete;
		PromptTerminalGuard(PromptTerminalGuard &&)                          = delete;
		auto operator=(PromptTerminalGuard &&) -> PromptTerminalGuard &      = delete;

		void MarkPromptShown() noexcept {
			prompt_shown_ = true;
		}

		void MarkLineRead() noexcept {
			line_read_ = true;
		}

		[[nodiscard]] auto Restore() -> bool {
			if (owner_ == nullptr) {
				return true;
			}
			auto *owner = std::exchange(owner_, nullptr);
			// Drop a half-typed secret so it cannot reach the next reader of the terminal: before
			// the restore (a non-canonical original would make it readable) and after it (keys
			// typed in between). Typeahead is kept when the prompt was never shown.
			const bool discard_input = prompt_shown_ && !line_read_;
			if (discard_input) {
				(void)tcflush(owner->tty_fd_.Get(), TCIFLUSH);
			}
			// Before the restore, while ONLCR is forced. With echo on, the kernel already echoed
			// the newline that ended the line.
			// After a face-match abort, output stopped with ^S (IXON) must not hang cleanup.
			if (!line_read_ || !echo_) {
				(void)WriteAll(owner->tty_fd_.Get(), "\n", owner->abort_requested_);
			}
			const bool restored = owner->RestorePromptTerminal(*original_);
			if (!restored) {
				owner->terminal_restore_failed_.store(true);
			}
			if (discard_input) {
				(void)tcflush(owner->tty_fd_.Get(), TCIFLUSH);
			}
			return restored;
		}

		~PromptTerminalGuard() {
			(void)Restore();
		}

	private:
		NativePromptConversation *owner_        = nullptr;
		const struct termios     *original_     = nullptr;
		bool                      echo_         = false;
		bool                      prompt_shown_ = false;
		bool                      line_read_    = false;
	} terminal_guard{this, &original_termios, !hide_input};

	const std::string prompt_text = message.msg == nullptr ? "" : message.msg;
	if (!WriteAll(tty_fd_.Get(), prompt_text, abort_requested_)) {
		return PAM_CONV_ERR;
	}
	terminal_guard.MarkPromptShown();

	howdy::pam::native_prompt_input::SensitiveBuffer password;
	bool                                             response_too_long = false;
	if (!ReadPromptPassword(original_termios, password, response_too_long)) {
		return PAM_CONV_ERR;
	}
	terminal_guard.MarkLineRead();
	if (!terminal_guard.Restore() || response_too_long) {
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
