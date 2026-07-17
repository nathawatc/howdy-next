#include "prompt/native_prompt_conversation.hpp"

#include "prompt/native_prompt_input.hpp"
#include "support/fd_io.hpp"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <syslog.h>
#include <termios.h>
#include <unistd.h>

#include <security/pam_appl.h>

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

	auto fail_closed_dispatch(int /*num_msg*/, const struct pam_message ** /*msgm*/,
	                          struct pam_response **response, void * /*appdata_ptr*/) -> int {
		if (response != nullptr) {
			*response = nullptr;
		}
		return PAM_CONV_ERR;
	}

	auto open_tty_fd(pam_handle_t *pamh) -> int {
		std::array<std::string, 2> candidates{};
		std::size_t                candidate_count = 0;

		const void *tty_item = nullptr;
		if (pam_get_item(pamh, PAM_TTY, &tty_item) == PAM_SUCCESS && tty_item != nullptr) {
			auto tty_path = std::string(static_cast<const char *>(tty_item));
			if (!tty_path.empty()) {
				if (tty_path.front() != '/') {
					tty_path = "/dev/" + tty_path;
				}
				candidates[candidate_count++] = std::move(tty_path);
			}
		}

		candidates[candidate_count++] = "/dev/tty";

		for (std::size_t i = 0; i < candidate_count; ++i) {
			const int fd = open(candidates[i].c_str(), O_RDWR | O_CLOEXEC | O_NOCTTY);
			if (fd >= 0) {
				return fd;
			}
		}

		return -1;
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

	void free_pam_responses(struct pam_response *responses, int num_msg) {
		if (responses == nullptr || num_msg <= 0) {
			return;
		}

		for (int index = 0; index < num_msg; ++index) {
			if (responses[index].resp == nullptr) {
				continue;
			}

			std::memset(responses[index].resp, 0, std::strlen(responses[index].resp));
			std::free(responses[index].resp);
			responses[index].resp = nullptr;
		}
		std::free(responses);
	}

}  // namespace

NativePromptConversation::NativePromptConversation(pam_handle_t *pamh)
    : pamh_(pamh)
    , override_conv_{.conv = dispatch, .appdata_ptr = this}
    , operations_(production_operations()) {
	const void *conv_ptr = nullptr;
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

	if (pipe2(abort_pipe_.data(), O_CLOEXEC | O_NONBLOCK) != 0) {
		close_fd(tty_fd_);
		return;
	}
}

auto NativePromptConversation::production_operations() -> Operations {
	return {
	    .poll_prompt      = production_poll,
	    .read_prompt      = production_read,
	    .restore_terminal = production_restore_terminal,
	    .post_message     = production_post_message,
	};
}

NativePromptConversation::NativePromptConversation(pam_handle_t   *pamh,
                                                   struct pam_conv original_conv,
                                                   bool has_original_conv, Descriptors descriptors,
                                                   Operations operations)
    : pamh_(pamh)
    , original_conv_(original_conv)
    , override_conv_{.conv = dispatch, .appdata_ptr = this}
    , has_original_conv_(has_original_conv)
    , tty_fd_(descriptors.tty_fd)
    , abort_pipe_{{descriptors.abort_read_fd, descriptors.abort_write_fd}}
    , operations_(operations) {}

NativePromptConversation::~NativePromptConversation() {
	restore_original();

	close_fd(tty_fd_);
	close_fd(abort_pipe_[0]);
	close_fd(abort_pipe_[1]);
}

void NativePromptConversation::restore_original() {
	if (!installed_) {
		return;
	}

	if (pamh_ == nullptr) {
		syslog(LOG_CRIT, "Cannot restore PAM conversation: null PAM handle");
		installed_ = false;
		return;
	}

	const int restore_result = pam_set_item(pamh_, PAM_CONV, &original_conv_);
	if (restore_result == PAM_SUCCESS) {
		installed_ = false;
		return;
	}

	syslog(LOG_CRIT, "Failed to restore original PAM conversation: %d", restore_result);
	static const struct pam_conv fail_closed_conv = {.conv        = fail_closed_dispatch,
	                                                 .appdata_ptr = nullptr};
	const int fail_closed_result = pam_set_item(pamh_, PAM_CONV, &fail_closed_conv);
	if (fail_closed_result != PAM_SUCCESS) {
		syslog(LOG_CRIT, "Failed to install fail-closed PAM conversation: %d", fail_closed_result);
		syslog(LOG_CRIT, "PAM_CONV may remain unsafe after native prompt restore failure");
		// Both PAM_CONV writes failed. There is no safe destructor-path recovery left;
		// make this object's state explicit so no later restore retry is implied.
		installed_ = false;
		return;
	}
	installed_ = false;
}

auto NativePromptConversation::available() const -> bool {
	return has_original_conv_ && tty_fd_ >= 0 && abort_pipe_[0] >= 0 && abort_pipe_[1] >= 0;
}

auto NativePromptConversation::install() -> int {
	if (!available()) {
		return PAM_SYSTEM_ERR;
	}

	const int pam_res = pam_set_item(pamh_, PAM_CONV, &override_conv_);
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

auto NativePromptConversation::dispatch(int num_msg, const struct pam_message **msgm,
                                        struct pam_response **response, void *appdata_ptr) -> int {
	if (response != nullptr) {
		*response = nullptr;
	}

	try {
		auto *self = static_cast<NativePromptConversation *>(appdata_ptr);
		if (self == nullptr || response == nullptr) {
			return PAM_CONV_ERR;
		}
		return self->handle(num_msg, msgm, response);
	} catch (const std::exception &error) {
		syslog(LOG_ERR, "Unhandled C++ exception in native PAM conversation: %s", error.what());
		if (response != nullptr) {
			free_pam_responses(*response, num_msg);
			*response = nullptr;
		}
		return PAM_CONV_ERR;
	} catch (...) {
		syslog(LOG_ERR, "Unhandled non-standard exception in native PAM conversation");
		if (response != nullptr) {
			free_pam_responses(*response, num_msg);
			*response = nullptr;
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
			free_pam_responses(pam_responses, num_msg);
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

		free_pam_responses(pam_responses, num_msg);
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
		(void)restore_prompt_terminal(original_termios);
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
