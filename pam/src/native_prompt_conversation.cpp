#include "native_prompt_conversation.hpp"

#include "common/fd_io.hpp"

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

	constexpr int         kAbortPollTimeoutMs     = 100;
	constexpr std::size_t kMaxPromptResponseBytes = 512;

	class SensitivePromptBuffer {
	public:
		SensitivePromptBuffer()                                                  = default;
		SensitivePromptBuffer(const SensitivePromptBuffer &)                     = delete;
		auto operator=(const SensitivePromptBuffer &) -> SensitivePromptBuffer & = delete;

		~SensitivePromptBuffer() {
			volatile char *cursor = data_.data();
			for (std::size_t index = 0; index < data_.size(); ++index) {
				cursor[index] = '\0';
			}
			length_ = 0;
		}

		[[nodiscard]] auto empty() const -> bool {
			return length_ == 0;
		}

		[[nodiscard]] auto full() const -> bool {
			return length_ == data_.size();
		}

		void push_back(char value) {
			data_[length_++] = value;
		}

		void pop_back() {
			if (length_ > 0) {
				data_[--length_] = '\0';
			}
		}

		[[nodiscard]] auto data() const -> const char * {
			return data_.data();
		}

		[[nodiscard]] auto size() const -> std::size_t {
			return length_;
		}

	private:
		std::array<char, kMaxPromptResponseBytes> data_{};
		std::size_t                               length_ = 0;
	};

#ifdef HOWDY_PAM_TESTING
	std::atomic<int> g_test_available_result{-1};
	std::atomic<int> g_test_install_result{-1};
#endif

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
    , override_conv_{.conv = dispatch, .appdata_ptr = this} {
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

#ifdef HOWDY_PAM_TESTING
NativePromptConversation::NativePromptConversation(int tty_fd, int abort_read_fd,
                                                   int abort_write_fd)
    : override_conv_{.conv = dispatch, .appdata_ptr = this}
    , has_original_conv_(true)
    , tty_fd_(tty_fd)
    , abort_pipe_{{abort_read_fd, abort_write_fd}} {}

void NativePromptConversation::set_test_throw_mode(int mode) {
	test_throw_mode_ = mode;
}

void NativePromptConversation::set_test_poll_eintr_count(int count) {
	test_poll_eintr_count_ = count < 0 ? 0 : count;
}

void NativePromptConversation::set_test_read_eintr_count(int count) {
	test_read_eintr_count_ = count < 0 ? 0 : count;
}

void NativePromptConversation::set_test_abort_on_poll_eintr(bool enabled) {
	test_abort_on_poll_eintr_ = enabled;
}

void NativePromptConversation::set_test_abort_on_read_eintr(bool enabled) {
	test_abort_on_read_eintr_ = enabled;
}

void NativePromptConversation::set_test_restore_failure(bool enabled) {
	test_restore_failure_ = enabled;
}

void NativePromptConversation::set_test_available_result(int result) {
	g_test_available_result.store(result);
}

void NativePromptConversation::set_test_install_result(int result) {
	g_test_install_result.store(result);
}
#endif

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
#ifdef HOWDY_PAM_TESTING
	const int test_result = g_test_available_result.load();
	if (test_result >= 0) {
		return test_result != 0;
	}
#endif
	return has_original_conv_ && tty_fd_ >= 0 && abort_pipe_[0] >= 0 && abort_pipe_[1] >= 0;
}

auto NativePromptConversation::install() -> int {
	if (!available()) {
		return PAM_SYSTEM_ERR;
	}

#ifdef HOWDY_PAM_TESTING
	const int test_result = g_test_install_result.load();
	if (test_result >= 0) {
		return test_result;
	}
#endif

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

#ifdef HOWDY_PAM_TESTING
		if (test_throw_mode_ == 1) {
			throw std::exception();
		}
		if (test_throw_mode_ == 2) {
			throw 1;
		}
#endif

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
	while (tcsetattr(tty_fd_, TCSANOW, &original_termios) != 0) {
		if (errno != EINTR) {
			return false;
		}
	}
#ifdef HOWDY_PAM_TESTING
	return !test_restore_failure_;
#else
	return true;
#endif
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
	const auto abort_prompt = [this, &original_termios] {
		(void)restore_prompt_terminal(original_termios);
		write_newline(tty_fd_);
		return PAM_CONV_ERR;
	};

	const std::string prompt_text = message.msg == nullptr ? "" : message.msg;
	if (!write_all(tty_fd_, prompt_text)) {
		return abort_prompt();
	}

	SensitivePromptBuffer        password;
	bool                         response_too_long = false;
	std::array<struct pollfd, 2> fds{{
	    {.fd = tty_fd_, .events = POLLIN, .revents = 0},
	    {.fd = abort_pipe_[0], .events = POLLIN, .revents = 0},
	}};

	while (true) {
#ifdef HOWDY_PAM_TESTING
		int poll_result = -1;
		if (test_poll_eintr_count_ > 0) {
			--test_poll_eintr_count_;
			if (test_abort_on_poll_eintr_) {
				abort_requested_.store(true);
			}
			errno = EINTR;
		} else {
			poll_result = poll(fds.data(), fds.size(), kAbortPollTimeoutMs);
		}
#else
		const int poll_result = poll(fds.data(), fds.size(), kAbortPollTimeoutMs);
#endif
		if (poll_result < 0) {
			if (errno == EINTR && !abort_requested_.load()) {
				continue;
			}
			return abort_prompt();
		}

		if (abort_requested_.load()) {
			drain_abort_pipe(abort_pipe_[0]);
			return abort_prompt();
		}

		constexpr short kFdFailureEvents = POLLHUP | POLLERR | POLLNVAL;
		if ((fds[0].revents & kFdFailureEvents) != 0 || (fds[1].revents & kFdFailureEvents) != 0) {
			return abort_prompt();
		}

		if ((fds[1].revents & POLLIN) != 0) {
			drain_abort_pipe(abort_pipe_[0]);
			return abort_prompt();
		}

		if (poll_result == 0 || (fds[0].revents & POLLIN) == 0) {
			continue;
		}

		char ch = '\0';
#ifdef HOWDY_PAM_TESTING
		ssize_t bytes_read = -1;
		if (test_read_eintr_count_ > 0) {
			--test_read_eintr_count_;
			if (test_abort_on_read_eintr_) {
				abort_requested_.store(true);
			}
			errno      = EINTR;
			bytes_read = -1;
		} else {
			bytes_read = read(tty_fd_, &ch, 1);
		}
#else
		const ssize_t bytes_read = read(tty_fd_, &ch, 1);
#endif
		if (bytes_read < 0) {
			if (errno == EINTR && !abort_requested_.load()) {
				continue;
			}
			return abort_prompt();
		}
		if (bytes_read == 0) {
			continue;
		}

		if (ch == '\n' || ch == '\r') {
			break;
		}

		if (ch == 3) {
			return abort_prompt();
		}

		if (ch == '\b' || ch == 127) {
			if (!password.empty()) {
				password.pop_back();
			}
			continue;
		}

		if (response_too_long) {
			continue;
		}
		if (password.full()) {
			response_too_long = true;
			continue;
		}
		password.push_back(ch);
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
