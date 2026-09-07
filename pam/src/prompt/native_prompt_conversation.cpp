#include "prompt/native_prompt_conversation.hpp"

#include "prompt/conversation_response.hpp"
#include "prompt/internal_fd.hpp"
#include "prompt/native_prompt_input.hpp"
#include "support/fd_io.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <poll.h>
#include <string>
#include <syslog.h>
#include <termios.h>
#include <unistd.h>
#include <vector>

#include <security/pam_appl.h>

#include <sys/ioctl.h>
#include <sys/stat.h>

namespace {

	constexpr int kAbortPollTimeoutMs = 100;

	auto production_poll(void * /*context*/, struct pollfd *fds, nfds_t count, int timeout) -> int {
		return poll(fds, count, timeout);
	}

	auto production_read(void * /*context*/, int fd, void *buffer, std::size_t count) -> ssize_t {
		return read(fd, buffer, count);
	}

	auto production_restore_terminal(void * /*context*/, int fd, const struct termios *termios)
	    -> int {
		return tcsetattr(fd, TCSANOW, termios);
	}

	void production_post_message(void * /*context*/) {}

	auto production_set_pam_item(void * /*context*/, pam_handle_t *pamh, int item_type,
	                             const void *item) -> int {
		return pam_set_item(pamh, item_type, item);
	}

	auto native_prompt_fail_closed_dispatch(int /*num_msg*/, const struct pam_message ** /*msgm*/,
	                                        struct pam_response **response, void * /*appdata_ptr*/)
	    -> int {
		if (response != nullptr) {
			*response = nullptr;
		}
		return PAM_CONV_ERR;
	}

	auto open_tty_fd(pam_handle_t *pamh) -> int {
		const void *tty_item = nullptr;
		if (pam_get_item(pamh, PAM_TTY, &tty_item) != PAM_SUCCESS || tty_item == nullptr) {
			return -1;
		}

		auto tty_path = std::string(static_cast<const char *>(tty_item));
		if (tty_path.empty()) {
			return -1;
		}
		if (tty_path.front() != '/') {
			tty_path = "/dev/" + tty_path;
		}

		auto tty_fd = howdy::pam::detail::normalize_internal_fd(
		    howdy::pam::detail::ScopedFd(open(tty_path.c_str(), O_RDWR | O_CLOEXEC | O_NOCTTY)));
		if (tty_fd.get() < 0) {
			return -1;
		}

		if (!native_prompt_terminal_is_interactive({.tty    = tty_fd.get(),
		                                            .input  = STDIN_FILENO,
		                                            .output = STDOUT_FILENO,
		                                            .error  = STDERR_FILENO})) {
			return -1;
		}
		return tty_fd.release();
	}

	void close_fd(int &fd) {
		if (fd >= 0) {
			close(fd);
			fd = -1;
		}
	}

	auto write_all(int fd, const std::string &text) -> bool {
		return howdy::native::write_all_to_fd(fd, text);
	}

	auto write_newline(int fd) -> void {
		constexpr char kNewline = '\n';
		while (write(fd, &kNewline, 1) < 0 && errno == EINTR) {
		}
	}

	void drain_abort_pipe(int fd) {
		std::array<char, 32> buffer{};
		while (read(fd, buffer.data(), buffer.size()) > 0) {
		}
	}

	auto same_terminal_identity(int target_fd, const struct stat &target_stat, int descriptor_fd)
	    -> bool {
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

auto native_prompt_terminal_is_interactive(const NativeTerminalDescriptors &descriptors) -> bool {
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
		                                  same_terminal_identity(descriptors.tty, tty_stat, fd);
	                           });
}

NativePromptConversation::NativePromptConversation(pam_handle_t *pamh)
    : pamh_(pamh)
    , dispatch_context_(std::make_unique<DispatchContext>())
    , override_conv_{.conv = dispatch, .appdata_ptr = dispatch_context_.get()}
    , operations_(production_operations()) {
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

	tty_fd_ = open_tty_fd(pamh_);
	if (tty_fd_ < 0) {
		return;
	}

	auto abort_pipe = howdy::pam::detail::create_internal_pipe(O_CLOEXEC | O_NONBLOCK);
	if (!abort_pipe.valid()) {
		close_fd(tty_fd_);
		return;
	}
	abort_pipe_[0] = abort_pipe.read.release();
	abort_pipe_[1] = abort_pipe.write.release();
}

auto NativePromptConversation::production_operations() -> Operations {
	return {
	    .poll_prompt      = production_poll,
	    .read_prompt      = production_read,
	    .restore_terminal = production_restore_terminal,
	    .post_message     = production_post_message,
	    .set_pam_item     = production_set_pam_item,
	};
}

NativePromptConversation::NativePromptConversation(pam_handle_t   *pamh,
                                                   struct pam_conv original_conv,
                                                   bool has_original_conv, Descriptors descriptors,
                                                   Operations operations)
    : pamh_(pamh)
    , original_conv_(original_conv)
    , dispatch_context_(std::make_unique<DispatchContext>())
    , override_conv_{.conv = dispatch, .appdata_ptr = dispatch_context_.get()}
    , has_original_conv_(has_original_conv)
    , tty_fd_(descriptors.tty_fd)
    , abort_pipe_{{descriptors.abort_read_fd, descriptors.abort_write_fd}}
    , operations_(operations) {
	dispatch_context_->owner = this;
}

NativePromptConversation::~NativePromptConversation() {
	if (installed_ && dispatch_context_ != nullptr) {
		retain_unsafe_dispatch_context();
	}
	close_fd(tty_fd_);
	close_fd(abort_pipe_[0]);
	close_fd(abort_pipe_[1]);
}

void NativePromptConversation::retain_unsafe_dispatch_context() noexcept {
	dispatch_context_->fail_closed.store(true);
	dispatch_context_->owner = nullptr;
	static std::mutex                                    quarantine_mutex;
	static std::vector<std::unique_ptr<DispatchContext>> quarantine;
	try {
		std::scoped_lock lock(quarantine_mutex);
		quarantine.push_back(std::move(dispatch_context_));
	} catch (...) {
		// Allocation failure cannot make stale PAM callback safe to free. Retain tiny
		// fail-closed context for process lifetime and report explicit ownership fallback.
		[[maybe_unused]] auto *retained_context = dispatch_context_.release();
		syslog(LOG_CRIT, "Native fail-closed callback context retained outside quarantine");
	}
}

auto NativePromptConversation::restore_original() noexcept
    -> howdy::pam::ConversationRestoreResult {
	if (!installed_) {
		return howdy::pam::ConversationRestoreResult::kOriginalRestored;
	}

	if (pamh_ == nullptr) {
		syslog(LOG_CRIT, "Cannot restore PAM conversation: null PAM handle");
		retain_unsafe_dispatch_context();
		installed_ = false;
		return howdy::pam::ConversationRestoreResult::kUnsafe;
	}

	const int restore_result =
	    operations_.set_pam_item(operations_.context, pamh_, PAM_CONV, &original_conv_);
	if (restore_result == PAM_SUCCESS) {
		installed_ = false;
		return howdy::pam::ConversationRestoreResult::kOriginalRestored;
	}

	syslog(LOG_CRIT, "Failed to restore original PAM conversation: %d", restore_result);
	static const struct pam_conv fail_closed_conv = {.conv = native_prompt_fail_closed_dispatch,
	                                                 .appdata_ptr = nullptr};
	const int                    fail_closed_result =
	    operations_.set_pam_item(operations_.context, pamh_, PAM_CONV, &fail_closed_conv);
	if (fail_closed_result != PAM_SUCCESS) {
		syslog(LOG_CRIT, "Failed to install fail-closed PAM conversation: %d", fail_closed_result);
		retain_unsafe_dispatch_context();
		installed_ = false;
		return howdy::pam::ConversationRestoreResult::kUnsafe;
	}
	installed_ = false;
	return howdy::pam::ConversationRestoreResult::kFailClosedInstalled;
}

auto NativePromptConversation::available() const -> bool {
	return has_original_conv_ && tty_fd_ >= 0 && abort_pipe_[0] >= 0 && abort_pipe_[1] >= 0;
}

auto NativePromptConversation::install() -> int {
	if (!available()) {
		return PAM_SYSTEM_ERR;
	}

	const int pam_res =
	    operations_.set_pam_item(operations_.context, pamh_, PAM_CONV, &override_conv_);
	if (pam_res == PAM_SUCCESS) {
		installed_ = true;
	}
	return pam_res;
}

void NativePromptConversation::request_abort() {
	abort_requested_.store(true);
	if (abort_pipe_[1] < 0) {
		return;
	}

	constexpr char kSignal = 'x';
	while (true) {
		const ssize_t result = write(abort_pipe_[1], &kSignal, 1);
		if (result == 1) {
			return;
		}
		if (result < 0 && errno == EINTR) {
			continue;
		}
		if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			return;
		}
		return;
	}
}

[[nodiscard]] auto NativePromptConversation::terminal_restore_failed() const noexcept -> bool {
	return terminal_restore_failed_.load();
}

auto NativePromptConversation::dispatch(int num_msg, const struct pam_message **msgm,
                                        struct pam_response **response, void *appdata_ptr) -> int {
	if (response != nullptr) {
		*response = nullptr;
	}

	try {
		auto *context = static_cast<DispatchContext *>(appdata_ptr);
		if (context == nullptr || response == nullptr || context->fail_closed.load() ||
		    context->owner == nullptr) {
			return PAM_CONV_ERR;
		}
		return context->owner->handle(num_msg, msgm, response);
	} catch (const std::exception &error) {
		syslog(LOG_ERR, "Unhandled C++ exception in native PAM conversation: %s", error.what());
		if (response != nullptr) {
			howdy::pam::secure_free_conversation_responses(response, num_msg);
		}
		return PAM_CONV_ERR;
	} catch (...) {
		syslog(LOG_ERR, "Unhandled non-standard exception in native PAM conversation");
		if (response != nullptr) {
			howdy::pam::secure_free_conversation_responses(response, num_msg);
		}
		return PAM_CONV_ERR;
	}
}

auto NativePromptConversation::handle(int num_msg, const struct pam_message **msgm,
                                      struct pam_response **response) -> int {
	if (num_msg <= 0 || msgm == nullptr || response == nullptr) {
		return PAM_CONV_ERR;
	}

	auto *pam_responses = static_cast<struct pam_response *>(
	    calloc(static_cast<std::size_t>(num_msg), sizeof(struct pam_response)));
	if (pam_responses == nullptr) {
		return PAM_BUF_ERR;
	}
	*response = pam_responses;

	for (int index = 0; index < num_msg; ++index) {
		if (msgm[index] == nullptr) {
			howdy::pam::secure_free_conversation_responses(&pam_responses, num_msg);
			*response = nullptr;
			return PAM_CONV_ERR;
		}

		const struct pam_message &message = *msgm[index];
		int                       result  = PAM_SUCCESS;
		switch (message.msg_style) {
			case PAM_PROMPT_ECHO_OFF:
				result = prompt_input(message, &pam_responses[index].resp, true);
				break;
			case PAM_PROMPT_ECHO_ON:
				result = prompt_input(message, &pam_responses[index].resp, false);
				break;
			case PAM_TEXT_INFO:
			case PAM_ERROR_MSG:
				result = write_message_line(message);
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

		howdy::pam::secure_free_conversation_responses(&pam_responses, num_msg);
		*response = nullptr;
		return result;
	}

	return PAM_SUCCESS;
}

auto NativePromptConversation::write_message_line(const struct pam_message &message) const -> int {
	if (tty_fd_ < 0) {
		return PAM_CONV_ERR;
	}

	const std::string text = message.msg == nullptr ? "" : message.msg;
	if (!write_all(tty_fd_, text)) {
		return PAM_CONV_ERR;
	}

	write_newline(tty_fd_);
	return PAM_SUCCESS;
}

auto NativePromptConversation::restore_prompt_terminal(const struct termios &original_termios) const
    -> bool {
	while (operations_.restore_terminal(operations_.context, tty_fd_, &original_termios) != 0) {
		if (errno != EINTR) {
			return false;
		}
	}
	return true;
}

auto NativePromptConversation::poll_prompt(std::array<struct pollfd, 2> &fds) const -> int {
	return operations_.poll_prompt(operations_.context, fds.data(), fds.size(),
	                               kAbortPollTimeoutMs);
}

auto NativePromptConversation::read_prompt_char(char *ch) const -> ssize_t {
	return operations_.read_prompt(operations_.context, tty_fd_, ch, 1);
}

auto NativePromptConversation::poll_prompt_state(std::array<struct pollfd, 2> &fds)
    -> PromptIoResult {
	const int poll_result = poll_prompt(fds);
	if (poll_result < 0) {
		return errno == EINTR && !abort_requested_.load() ? PromptIoResult::retry
		                                                  : PromptIoResult::abort;
	}
	if (abort_requested_.load()) {
		drain_abort_pipe(abort_pipe_[0]);
		return PromptIoResult::abort;
	}
	constexpr short kFdFailureEvents = POLLHUP | POLLERR | POLLNVAL;
	if ((fds[0].revents & kFdFailureEvents) != 0 || (fds[1].revents & kFdFailureEvents) != 0) {
		return PromptIoResult::abort;
	}
	if ((fds[1].revents & POLLIN) != 0) {
		drain_abort_pipe(abort_pipe_[0]);
		return PromptIoResult::abort;
	}
	return poll_result == 0 || (fds[0].revents & POLLIN) == 0 ? PromptIoResult::retry
	                                                          : PromptIoResult::ready;
}

auto NativePromptConversation::read_prompt_state(char *ch) -> PromptIoResult {
	const ssize_t bytes_read = read_prompt_char(ch);
	if (bytes_read < 0) {
		return errno == EINTR && !abort_requested_.load() ? PromptIoResult::retry
		                                                  : PromptIoResult::abort;
	}
	return bytes_read == 0 ? PromptIoResult::retry : PromptIoResult::ready;
}

auto NativePromptConversation::wait_for_prompt_character(char *ch) -> PromptIoResult {
	std::array<struct pollfd, 2> fds{{
	    {.fd = tty_fd_, .events = POLLIN, .revents = 0},
	    {.fd = abort_pipe_[0], .events = POLLIN, .revents = 0},
	}};
	while (true) {
		const auto poll_result = poll_prompt_state(fds);
		if (poll_result == PromptIoResult::abort) {
			return poll_result;
		}
		if (poll_result == PromptIoResult::retry) {
			continue;
		}
		const auto read_result = read_prompt_state(ch);
		if (read_result != PromptIoResult::retry) {
			return read_result;
		}
	}
}

auto NativePromptConversation::prompt_input(const struct pam_message &message, char **response,
                                            bool hide_input) -> int {
	if (response == nullptr || tty_fd_ < 0) {
		return PAM_CONV_ERR;
	}

	if (abort_requested_.load()) {
		return PAM_CONV_ERR;
	}

	struct termios original_termios{};
	if (tcgetattr(tty_fd_, &original_termios) != 0) {
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
	if (tcsetattr(tty_fd_, TCSANOW, &prompt_termios) != 0) {
		return PAM_CONV_ERR;
	}
	const auto abort_prompt = [this, &original_termios] -> int {
		const bool restored = restore_prompt_terminal(original_termios);
		if (!restored) {
			terminal_restore_failed_.store(true);
		}
		write_newline(tty_fd_);
		return PAM_CONV_ERR;
	};

	const std::string prompt_text = message.msg == nullptr ? "" : message.msg;
	if (!write_all(tty_fd_, prompt_text)) {
		return abort_prompt();
	}

	howdy::pam::native_prompt_input::SensitiveBuffer password;
	bool                                             response_too_long = false;

	while (true) {
		char ch = '\0';
		if (wait_for_prompt_character(&ch) == PromptIoResult::abort) {
			return abort_prompt();
		}

		const auto character_result =
		    howdy::pam::native_prompt_input::process_character(ch, password, response_too_long);
		if (character_result == howdy::pam::native_prompt_input::CharacterResult::complete) {
			break;
		}
		if (character_result == howdy::pam::native_prompt_input::CharacterResult::abort) {
			return abort_prompt();
		}
	}

	if (!restore_prompt_terminal(original_termios)) {
		terminal_restore_failed_.store(true);
		write_newline(tty_fd_);
		return PAM_CONV_ERR;
	}
	write_newline(tty_fd_);
	if (response_too_long) {
		return PAM_CONV_ERR;
	}

	auto *pam_response = static_cast<struct pam_response *>(calloc(1, sizeof(struct pam_response)));
	if (pam_response == nullptr) {
		return PAM_BUF_ERR;
	}

	pam_response->resp = static_cast<char *>(calloc(password.size() + 1, sizeof(char)));
	if (pam_response->resp == nullptr) {
		free(pam_response);
		return PAM_BUF_ERR;
	}

	std::memcpy(pam_response->resp, password.data(), password.size());
	pam_response->resp_retcode = 0;
	*response                  = pam_response->resp;
	std::free(pam_response);
	return PAM_SUCCESS;
}
