#pragma once

#include "prompt/native_prompt_conversation.hpp"
#include "test_support.hpp"

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <memory>
#include <poll.h>
#include <unistd.h>

class NativePromptConversationTestAccess {
public:
	struct Descriptors {
		int tty_fd         = -1;
		int abort_read_fd  = -1;
		int abort_write_fd = -1;
	};

	struct Operations {
		void *context                                                  = nullptr;
		int (*poll_prompt)(void *, struct pollfd *, nfds_t, int)       = nullptr;
		ssize_t (*read_prompt)(void *, int, void *, std::size_t)       = nullptr;
		int (*restore_terminal)(void *, int, const struct termios *)   = nullptr;
		void (*post_message)(void *)                                   = nullptr;
		int (*set_pam_item)(void *, pam_handle_t *, int, const void *) = nullptr;
	};

	static auto create(Descriptors descriptors) -> std::unique_ptr<NativePromptConversation> {
		return create(descriptors, Operations{});
	}

	static auto create(Descriptors descriptors, Operations operations)
	    -> std::unique_ptr<NativePromptConversation> {
		auto production = NativePromptConversation::production_operations();
		NativePromptConversation::Operations injected{
		    .context = operations.context,
		    .poll_prompt =
		        operations.poll_prompt != nullptr ? operations.poll_prompt : production.poll_prompt,
		    .read_prompt =
		        operations.read_prompt != nullptr ? operations.read_prompt : production.read_prompt,
		    .restore_terminal = operations.restore_terminal != nullptr
		                            ? operations.restore_terminal
		                            : production.restore_terminal,
		    .post_message     = operations.post_message != nullptr ? operations.post_message
		                                                           : production.post_message,
		    .set_pam_item     = operations.set_pam_item != nullptr ? operations.set_pam_item
		                                                           : production.set_pam_item,
		};
		return std::unique_ptr<NativePromptConversation>(
		    new NativePromptConversation(nullptr, {}, true,
		                                 {.tty_fd         = descriptors.tty_fd,
		                                  .abort_read_fd  = descriptors.abort_read_fd,
		                                  .abort_write_fd = descriptors.abort_write_fd},
		                                 injected));
	}

	static auto dispatch(int num_msg, const struct pam_message **messages,
	                     struct pam_response **response, void *appdata_ptr) -> int {
		auto *conversation = static_cast<NativePromptConversation *>(appdata_ptr);
		return NativePromptConversation::dispatch(
		    num_msg, messages, response,
		    conversation == nullptr ? nullptr : conversation->dispatch_context_.get());
	}

	static auto prompt_input(NativePromptConversation &conversation,
	                         const struct pam_message &message, char **response, bool hide_input)
	    -> int {
		return conversation.prompt_input(message, response, hide_input);
	}

	[[nodiscard]] static auto terminal_restore_failed(const NativePromptConversation &conversation)
	    -> bool {
		return conversation.terminal_restore_failed_.load();
	}

	static void replace_descriptors(NativePromptConversation &conversation,
	                                Descriptors               descriptors) {
		conversation.tty_fd_     = descriptors.tty_fd;
		conversation.abort_pipe_ = {descriptors.abort_read_fd, descriptors.abort_write_fd};
	}

	static void close_abort_write_fd(NativePromptConversation &conversation) {
		::close(conversation.abort_pipe_[1]);
		conversation.abort_pipe_[1] = -1;
	}

	static void set_installed(NativePromptConversation &conversation, bool installed) {
		conversation.installed_ = installed;
	}

	static void set_pam_handle(NativePromptConversation &conversation, pam_handle_t *pamh) {
		conversation.pamh_ = pamh;
	}

	static auto override_conversation(const NativePromptConversation &conversation)
	    -> struct pam_conv {
		return conversation.override_conv_;

	}

	[[nodiscard]] static auto
	installed(const NativePromptConversation &conversation) -> bool {
		return conversation.installed_;
	}

	[[nodiscard]] static auto tty_fd(const NativePromptConversation &conversation) -> int {
		return conversation.tty_fd_;
	}

	[[nodiscard]] static auto abort_read_fd(const NativePromptConversation &conversation) -> int {
		return conversation.abort_pipe_[0];
	}

	[[nodiscard]] static auto abort_write_fd(const NativePromptConversation &conversation) -> int {
		return conversation.abort_pipe_[1];
	}
};

using ScopedFd = howdy::test::ScopedFd;

inline auto open_pty_pair(ScopedFd *master_fd, ScopedFd *slave_fd) -> bool {
	master_fd->reset(posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC));
	if (master_fd->get() < 0) {
		return false;
	}

	if (grantpt(master_fd->get()) != 0 || unlockpt(master_fd->get()) != 0) {
		master_fd->reset();
		return false;
	}

	char *slave_name = ptsname(master_fd->get());
	if (slave_name == nullptr) {
		master_fd->reset();
		return false;
	}

	slave_fd->reset(open(slave_name, O_RDWR | O_NOCTTY | O_CLOEXEC));
	if (slave_fd->get() < 0) {
		master_fd->reset();
		return false;
	}

	return true;
}

inline auto open_pipe(std::array<ScopedFd, 2> *fds) -> bool {
	std::array<int, 2> raw_fds{{-1, -1}};
	if (pipe2(raw_fds.data(), O_CLOEXEC | O_NONBLOCK) != 0) {
		return false;
	}
	(*fds)[0].reset(raw_fds[0]);
	(*fds)[1].reset(raw_fds[1]);
	return true;
}

inline constexpr int kPromptReadTimeoutMs = 1000;

struct ReadBuffer {
	char       *data = nullptr;
	std::size_t size = 0;
};

inline auto read_with_timeout(int fd, ReadBuffer buffer, int timeout_ms) -> ssize_t {
	struct pollfd poll_fd{.fd = fd, .events = POLLIN, .revents = 0};

	while (true) {
		const int poll_result = poll(&poll_fd, 1, timeout_ms);
		if (poll_result < 0 && errno == EINTR) {
			continue;
		}
		if (poll_result == 0) {
			return 0;
		}
		if (poll_result < 0 || (poll_fd.revents & POLLIN) == 0) {
			return -1;
		}

		while (true) {
			const ssize_t bytes_read = read(fd, buffer.data, buffer.size);
			if (bytes_read < 0 && errno == EINTR) {
				continue;
			}
			return bytes_read;
		}
	}
}

inline auto create_conversation(NativePromptConversationTestAccess::Descriptors descriptors,
                                NativePromptConversationTestAccess::Operations  operations = {})
    -> std::unique_ptr<NativePromptConversation> {
	return NativePromptConversationTestAccess::create(descriptors, operations);
}
