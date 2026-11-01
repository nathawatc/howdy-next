#include "pam_test_support.hpp"
#include "prompt/native_prompt_conversation.hpp"
#include "prompt/native_prompt_input.hpp"
#include "prompt/native_prompt_test_groups.hpp"
#include "prompt/native_prompt_test_support.hpp"
#include "support/bit_flags.hpp"
#include "support/fd_io.hpp"
#include "test_support.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#include <sys/poll.h>
#include <sys/types.h>

using howdy::test::Expect;

namespace {
	struct InputOperationContext {
		NativePromptConversation *conversation        = nullptr;
		int                       poll_eintr_count    = 0;
		int                       read_eintr_count    = 0;
		int                       read_zero_count     = 0;
		int                       read_eagain_count   = 0;
		int                       restore_eintr_count = 0;
		bool                      abort_on_poll       = false;
		bool                      abort_on_read       = false;
		bool                      throw_on_poll       = false;
		bool                      restore_failure     = false;
	};

	auto InjectedPoll(void *context, struct pollfd *fds, nfds_t count, int timeout) -> int {
		auto &operations = *static_cast<InputOperationContext *>(context);
		if (operations.throw_on_poll) {
			throw std::runtime_error("simulated poll exception");
		}
		if (operations.poll_eintr_count > 0) {
			--operations.poll_eintr_count;
			if (operations.abort_on_poll) {
				operations.conversation->RequestAbort();
			}
			errno = EINTR;
			return -1;
		}
		return poll(fds, count, timeout);
	}

	auto InjectedRead(void *context, int fd, void *buffer, std::size_t count) -> ssize_t {
		auto &operations = *static_cast<InputOperationContext *>(context);
		if (operations.read_eintr_count > 0) {
			--operations.read_eintr_count;
			if (operations.abort_on_read) {
				operations.conversation->RequestAbort();
			}
			errno = EINTR;
			return -1;
		}
		if (operations.read_eagain_count > 0) {
			--operations.read_eagain_count;
			if (operations.abort_on_read) {
				operations.conversation->RequestAbort();
			}
			errno = EAGAIN;
			return -1;
		}
		if (operations.read_zero_count > 0) {
			--operations.read_zero_count;
			return 0;
		}
		return read(fd, buffer, count);
	}

	auto InjectedRestore(void *context, int fd, const struct termios *termios) -> int {
		auto &operations = *static_cast<InputOperationContext *>(context);
		if (operations.restore_eintr_count > 0) {
			--operations.restore_eintr_count;
			errno = EINTR;
			return -1;
		}
		if (operations.restore_failure) {
			errno = EIO;
			return -1;
		}
		return tcsetattr(fd, TCSANOW, termios);
	}

	void NativePromptInputInjectedPostMessage(void * /*context*/) {}

	auto CreateInputConversation(NativePromptConversationTestAccess::Descriptors descriptors,
	                             InputOperationContext *operations = nullptr)
	    -> std::unique_ptr<NativePromptConversation> {
		auto conversation = CreateConversation(
		    std::move(descriptors), operations == nullptr
		                                ? NativePromptConversationTestAccess::Operations{}
		                                : NativePromptConversationTestAccess::Operations{
		                                      .context          = operations,
		                                      .poll_prompt      = InjectedPoll,
		                                      .read_prompt      = InjectedRead,
		                                      .restore_terminal = InjectedRestore,
		                                      .post_message = NativePromptInputInjectedPostMessage,
		                                  });
		if (operations != nullptr) {
			operations->conversation = conversation.get();
		}
		return conversation;
	}

	auto WriteAll(int fd, std::string_view data) -> bool {
		return howdy::native::WriteAllToFd(fd, data);
	}

	auto ExpectPromptInputReturnsPasswordAfterRetry(NativePromptConversation *conversation,
	                                                int master_fd, const std::string &message)
	    -> bool {
		bool ok = true;

		const struct pam_message prompt = {
		    .msg_style = PAM_PROMPT_ECHO_OFF,
		    .msg       = "Password: ",
		};

		int         prompt_result = PAM_SUCCESS;
		char       *response      = nullptr;
		std::thread prompt_thread([&] -> void {
			prompt_result = NativePromptConversationTestAccess::PromptInput(*conversation, prompt,
			                                                                &response, true);
		});

		std::array<char, 64> prompt_buffer{};
		const ssize_t        prompt_bytes =
		    ReadWithTimeout(master_fd, {.data = prompt_buffer.data(), .size = prompt_buffer.size()},
			                kPromptReadTimeoutMs);
		ok &= Expect(prompt_bytes > 0, message + ": prompt is written to tty");

		constexpr std::array<char, 7> password{'s', 'e', 'c', 'r', 'e', 't', '\n'};
		ok &= Expect(write(master_fd, password.data(), password.size()) ==
		                 static_cast<ssize_t>(password.size()),
		             message + ": writes password response");

		prompt_thread.join();

		ok &= Expect(prompt_result == PAM_SUCCESS, message + ": prompt succeeds after EINTR");
		ok &= Expect(response != nullptr, message + ": prompt returns response");
		if (response != nullptr) {
			ok &= Expect(std::string(response) == "secret", message + ": response matches input");
			std::free(response);
		}
		return ok;
	}
}  // namespace

auto ExpectCtrlCAbortsPromptAndRestoresTerminal() -> bool {
	bool ok = true;

	ScopedFd                master_fd;
	ScopedFd                slave_fd;
	std::array<ScopedFd, 2> abort_pipe;

	ok &= Expect(OpenPtyPair(&master_fd, &slave_fd), "opens pseudo terminal");
	ok &= Expect(OpenPipe(&abort_pipe), "creates abort pipe");
	if (!ok) {
		return false;
	}

	const struct pam_message message = {
	    .msg_style = PAM_PROMPT_ECHO_OFF,
	    .msg       = "Password: ",
	};

	const int slave_raw_fd = slave_fd.Get();
	auto      conversation = CreateConversation({
	    .tty_fd         = std::move(slave_fd),
	    .abort_read_fd  = std::move(abort_pipe.at(0)),
	    .abort_write_fd = std::move(abort_pipe.at(1)),
	});

	int         prompt_result = PAM_SUCCESS;
	char       *response      = nullptr;
	std::thread prompt_thread([&] -> void {
		prompt_result = NativePromptConversationTestAccess::PromptInput(*conversation, message,
		                                                                &response, true);
	});

	std::array<char, 64> prompt_buffer{};
	const ssize_t        prompt_bytes = ReadWithTimeout(
	    master_fd.Get(), {.data = prompt_buffer.data(), .size = prompt_buffer.size()},
	    kPromptReadTimeoutMs);
	ok &= Expect(prompt_bytes > 0, "prompt is written to tty");

	constexpr char ctrl_c = 3;
	ok &= Expect(write(master_fd.Get(), &ctrl_c, 1) == 1, "writes Ctrl-C byte to pseudo terminal");

	prompt_thread.join();

	ok &= Expect(prompt_result == PAM_CONV_ERR, "Ctrl-C byte aborts the native prompt");
	ok &= Expect(!NativePromptConversationTestAccess::TerminalRestoreFailed(*conversation),
	             "successful terminal restoration keeps cancellation non-fatal");
	ok &= Expect(response == nullptr, "aborted prompt does not return a response");

	struct termios restored_termios{};
	ok &= Expect(tcgetattr(slave_raw_fd, &restored_termios) == 0,
	             "terminal state remains readable after abort");
	ok &= Expect((restored_termios.c_lflag & ICANON) != 0, "canonical mode restored after abort");
	ok &= Expect((restored_termios.c_lflag & ECHO) != 0, "echo restored after abort");
	ok &= Expect((restored_termios.c_lflag & ISIG) != 0, "signal generation restored after abort");

	if (response != nullptr) {
		std::free(response);
	}

	return ok;
}

auto ExpectAbortRequestUnblocksWithoutPipeWakeup() -> bool {
	bool ok = true;

	ScopedFd                master_fd;
	ScopedFd                slave_fd;
	std::array<ScopedFd, 2> abort_pipe;
	ok &= Expect(OpenPtyPair(&master_fd, &slave_fd), "abort wake test opens pseudo terminal");
	ok &= Expect(OpenPipe(&abort_pipe), "abort wake test creates abort pipe");
	if (!ok) {
		return false;
	}

	const struct pam_message message = {
	    .msg_style = PAM_PROMPT_ECHO_OFF,
	    .msg       = "Password: ",
	};

	InputOperationContext operations{.restore_failure = true};
	auto const conversation = std::shared_ptr<NativePromptConversation>(CreateInputConversation(
	    {
	        .tty_fd         = std::move(slave_fd),
	        .abort_read_fd  = std::move(abort_pipe.at(0)),
	        .abort_write_fd = std::move(abort_pipe.at(1)),
	    },
	    &operations));
	NativePromptConversationTestAccess::CloseAbortWriteFd(*conversation);

	auto const   response       = std::make_shared<char *>(nullptr);
	auto const   result_promise = std::make_shared<std::promise<int>>();
	auto         result_future  = result_promise->get_future();
	std::jthread prompt_thread([conversation, message, response, result_promise] -> void {
		result_promise->set_value(NativePromptConversationTestAccess::PromptInput(
		    *conversation, message, response.get(), true));
	});

	std::array<char, 64> prompt_buffer{};
	const ssize_t        prompt_bytes = ReadWithTimeout(
	    master_fd.Get(), {.data = prompt_buffer.data(), .size = prompt_buffer.size()},
	    kPromptReadTimeoutMs);
	ok &= Expect(prompt_bytes > 0, "abort wake test prompt is written to tty");

	conversation->RequestAbort();
	if (result_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
		master_fd.Reset();
		conversation->RequestAbort();
	}
	if (result_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
		(void)Expect(false, "abort wake test prompt thread stops before timeout");
		_exit(EXIT_FAILURE);
	}
	conversation->RequestAbort();
	prompt_thread.join();

	const int prompt_result = result_future.get();
	ok &= Expect(prompt_result == PAM_CONV_ERR,
	             "abort wake test terminal restore failure returns conversation error");
	ok &= Expect(NativePromptConversationTestAccess::TerminalRestoreFailed(*conversation),
	             "abort wake test records terminal restore failure");
	ok &= Expect(*response == nullptr, "abort wake test returns no response");
	if (*response != nullptr) {
		std::free(*response);
	}
	return ok;
}

auto ExpectPtyHangupAbortsPrompt() -> bool {
	bool ok = true;

	ScopedFd                master_fd;
	ScopedFd                slave_fd;
	std::array<ScopedFd, 2> abort_pipe;
	ok &= Expect(OpenPtyPair(&master_fd, &slave_fd), "hangup test opens pseudo terminal");
	ok &= Expect(OpenPipe(&abort_pipe), "hangup test creates abort pipe");
	if (!ok) {
		return false;
	}

	const struct pam_message message = {
	    .msg_style = PAM_PROMPT_ECHO_OFF,
	    .msg       = "Password: ",
	};

	auto const conversation = std::shared_ptr<NativePromptConversation>(CreateConversation({
	    .tty_fd         = std::move(slave_fd),
	    .abort_read_fd  = std::move(abort_pipe.at(0)),
	    .abort_write_fd = std::move(abort_pipe.at(1)),
	}));

	auto const   response       = std::make_shared<char *>(nullptr);
	auto const   result_promise = std::make_shared<std::promise<int>>();
	auto         result_future  = result_promise->get_future();
	std::jthread prompt_thread([conversation, message, response, result_promise] -> void {
		result_promise->set_value(NativePromptConversationTestAccess::PromptInput(
		    *conversation, message, response.get(), true));
	});

	std::array<char, 64> prompt_buffer{};
	const ssize_t        prompt_bytes = ReadWithTimeout(
	    master_fd.Get(), {.data = prompt_buffer.data(), .size = prompt_buffer.size()},
	    kPromptReadTimeoutMs);
	ok &= Expect(prompt_bytes > 0, "hangup test prompt is written to tty");

	master_fd.Reset();
	if (result_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
		conversation->RequestAbort();
	}
	if (result_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
		(void)Expect(false, "PTY hangup prompt thread stops before timeout");
		_exit(EXIT_FAILURE);
	}
	conversation->RequestAbort();
	prompt_thread.join();

	const int prompt_result = result_future.get();
	ok &= Expect(prompt_result == PAM_CONV_ERR, "PTY hangup aborts prompt");
	ok &= Expect(*response == nullptr, "PTY hangup returns no response");
	if (*response != nullptr) {
		std::free(*response);
	}
	return ok;
}

auto ExpectPollEintrWithoutAbortDoesNotAbortPrompt() -> bool {
	bool                    ok = true;
	ScopedFd                master_fd;
	ScopedFd                slave_fd;
	std::array<ScopedFd, 2> abort_pipe;

	ok &= Expect(OpenPtyPair(&master_fd, &slave_fd), "poll EINTR retry test opens pseudo terminal");
	ok &= Expect(OpenPipe(&abort_pipe), "poll EINTR retry test creates abort pipe");
	if (!ok) {
		return false;
	}

	InputOperationContext operations{.poll_eintr_count = 1};
	auto const            conversation = CreateInputConversation(
	    {
	        .tty_fd         = std::move(slave_fd),
	        .abort_read_fd  = std::move(abort_pipe.at(0)),
	        .abort_write_fd = std::move(abort_pipe.at(1)),
	    },
	    &operations);
	return ExpectPromptInputReturnsPasswordAfterRetry(conversation.get(), master_fd.Get(),
	                                                  "poll EINTR retry test");
}

auto ExpectPollEintrWithAbortFailsClosed() -> bool {
	bool                    ok = true;
	ScopedFd                master_fd;
	ScopedFd                slave_fd;
	std::array<ScopedFd, 2> abort_pipe;

	ok &= Expect(OpenPtyPair(&master_fd, &slave_fd), "poll EINTR abort test opens pseudo terminal");
	ok &= Expect(OpenPipe(&abort_pipe), "poll EINTR abort test creates abort pipe");
	if (!ok) {
		return false;
	}

	const struct pam_message prompt = {
	    .msg_style = PAM_PROMPT_ECHO_OFF,
	    .msg       = "Password: ",
	};

	InputOperationContext operations{.poll_eintr_count = 1, .abort_on_poll = true};
	auto                  conversation = CreateInputConversation(
	    {
	        .tty_fd         = std::move(slave_fd),
	        .abort_read_fd  = std::move(abort_pipe.at(0)),
	        .abort_write_fd = std::move(abort_pipe.at(1)),
	    },
	    &operations);

	int         prompt_result = PAM_SUCCESS;
	char       *response      = nullptr;
	std::thread prompt_thread([&] -> void {
		prompt_result =
		    NativePromptConversationTestAccess::PromptInput(*conversation, prompt, &response, true);
	});

	std::array<char, 64> prompt_buffer{};
	const ssize_t        prompt_bytes = ReadWithTimeout(
	    master_fd.Get(), {.data = prompt_buffer.data(), .size = prompt_buffer.size()},
	    kPromptReadTimeoutMs);
	ok &= Expect(prompt_bytes > 0, "poll EINTR abort test prompt is written to tty");

	prompt_thread.join();

	ok &= Expect(prompt_result == PAM_CONV_ERR, "poll EINTR abort test fails closed");
	ok &= Expect(response == nullptr, "poll EINTR abort test returns no response");
	if (response != nullptr) {
		std::free(response);
	}
	return ok;
}

auto ExpectReadEintrRetriesAndAcceptsInput() -> bool {
	bool                    ok = true;
	ScopedFd                master_fd;
	ScopedFd                slave_fd;
	std::array<ScopedFd, 2> abort_pipe;

	ok &= Expect(OpenPtyPair(&master_fd, &slave_fd), "read EINTR retry test opens pseudo terminal");
	ok &= Expect(OpenPipe(&abort_pipe), "read EINTR retry test creates abort pipe");
	if (!ok) {
		return false;
	}

	InputOperationContext operations{.read_eintr_count = 1};
	auto const            conversation = CreateInputConversation(
	    {
	        .tty_fd         = std::move(slave_fd),
	        .abort_read_fd  = std::move(abort_pipe.at(0)),
	        .abort_write_fd = std::move(abort_pipe.at(1)),
	    },
	    &operations);
	return ExpectPromptInputReturnsPasswordAfterRetry(conversation.get(), master_fd.Get(),
	                                                  "read EINTR retry test");
}

auto ExpectReadEintrWithAbortFailsClosed() -> bool {
	bool                    ok = true;
	ScopedFd                master_fd;
	ScopedFd                slave_fd;
	std::array<ScopedFd, 2> abort_pipe;

	ok &= Expect(OpenPtyPair(&master_fd, &slave_fd), "read EINTR abort test opens pseudo terminal");
	ok &= Expect(OpenPipe(&abort_pipe), "read EINTR abort test creates abort pipe");
	if (!ok) {
		return false;
	}

	const struct pam_message prompt = {
	    .msg_style = PAM_PROMPT_ECHO_OFF,
	    .msg       = "Password: ",
	};

	InputOperationContext operations{.read_eintr_count = 1, .abort_on_read = true};
	auto                  conversation = CreateInputConversation(
	    {
	        .tty_fd         = std::move(slave_fd),
	        .abort_read_fd  = std::move(abort_pipe.at(0)),
	        .abort_write_fd = std::move(abort_pipe.at(1)),
	    },
	    &operations);

	int         prompt_result = PAM_SUCCESS;
	char       *response      = nullptr;
	std::thread prompt_thread([&] -> void {
		prompt_result =
		    NativePromptConversationTestAccess::PromptInput(*conversation, prompt, &response, true);
	});

	std::array<char, 64> prompt_buffer{};
	const ssize_t        prompt_bytes = ReadWithTimeout(
	    master_fd.Get(), {.data = prompt_buffer.data(), .size = prompt_buffer.size()},
	    kPromptReadTimeoutMs);
	ok &= Expect(prompt_bytes > 0, "read EINTR abort test prompt is written to tty");

	ok &= Expect(WriteAll(master_fd.Get(), "s\n"), "read EINTR abort test makes tty readable");

	prompt_thread.join();

	ok &= Expect(prompt_result == PAM_CONV_ERR, "read EINTR abort test fails closed");
	ok &= Expect(response == nullptr, "read EINTR abort test returns no response");
	if (response != nullptr) {
		std::free(response);
	}
	return ok;
}

auto ExpectReadZeroAbortsPrompt() -> bool {
	bool                    ok = true;
	ScopedFd                master_fd;
	ScopedFd                slave_fd;
	std::array<ScopedFd, 2> abort_pipe;

	ok &= Expect(OpenPtyPair(&master_fd, &slave_fd), "read zero test opens pseudo terminal");
	ok &= Expect(OpenPipe(&abort_pipe), "read zero test creates abort pipe");
	if (!ok) {
		return false;
	}

	const struct pam_message prompt = {
	    .msg_style = PAM_PROMPT_ECHO_OFF,
	    .msg       = "Password: ",
	};

	InputOperationContext operations{.read_zero_count = 1};
	auto const            conversation = CreateInputConversation(
	    {
	        .tty_fd         = std::move(slave_fd),
	        .abort_read_fd  = std::move(abort_pipe.at(0)),
	        .abort_write_fd = std::move(abort_pipe.at(1)),
	    },
	    &operations);

	int         prompt_result = PAM_SUCCESS;
	char       *response      = nullptr;
	std::thread prompt_thread([&] -> void {
		prompt_result =
		    NativePromptConversationTestAccess::PromptInput(*conversation, prompt, &response, true);
	});

	std::array<char, 64> prompt_buffer{};
	ok &= Expect(ReadWithTimeout(master_fd.Get(),
	                             {.data = prompt_buffer.data(), .size = prompt_buffer.size()},
	                             kPromptReadTimeoutMs) > 0,
	             "read zero test prompt is written to tty");
	ok &= Expect(WriteAll(master_fd.Get(), "secret\n"), "read zero test makes tty readable");

	prompt_thread.join();

	ok &= Expect(prompt_result == PAM_CONV_ERR, "read returning EOF aborts the prompt");
	ok &= Expect(response == nullptr, "read zero test returns no response");
	if (response != nullptr) {
		std::free(response);
	}
	return ok;
}

auto ExpectReadEagainRetriesAndAcceptsInput() -> bool {
	bool                    ok = true;
	ScopedFd                master_fd;
	ScopedFd                slave_fd;
	std::array<ScopedFd, 2> abort_pipe;

	ok &=
	    Expect(OpenPtyPair(&master_fd, &slave_fd), "read EAGAIN retry test opens pseudo terminal");
	ok &= Expect(OpenPipe(&abort_pipe), "read EAGAIN retry test creates abort pipe");
	if (!ok) {
		return false;
	}

	InputOperationContext operations{.read_eagain_count = 1};
	auto const            conversation = CreateInputConversation(
	    {
	        .tty_fd         = std::move(slave_fd),
	        .abort_read_fd  = std::move(abort_pipe.at(0)),
	        .abort_write_fd = std::move(abort_pipe.at(1)),
	    },
	    &operations);
	return ExpectPromptInputReturnsPasswordAfterRetry(conversation.get(), master_fd.Get(),
	                                                  "read EAGAIN retry test");
}

auto ExpectReadEagainWithAbortFailsClosed() -> bool {
	bool                    ok = true;
	ScopedFd                master_fd;
	ScopedFd                slave_fd;
	std::array<ScopedFd, 2> abort_pipe;

	ok &=
	    Expect(OpenPtyPair(&master_fd, &slave_fd), "read EAGAIN abort test opens pseudo terminal");
	ok &= Expect(OpenPipe(&abort_pipe), "read EAGAIN abort test creates abort pipe");
	if (!ok) {
		return false;
	}

	const struct pam_message prompt = {
	    .msg_style = PAM_PROMPT_ECHO_OFF,
	    .msg       = "Password: ",
	};

	InputOperationContext operations{.read_eagain_count = 1, .abort_on_read = true};
	auto                  conversation = CreateInputConversation(
	    {
	        .tty_fd         = std::move(slave_fd),
	        .abort_read_fd  = std::move(abort_pipe.at(0)),
	        .abort_write_fd = std::move(abort_pipe.at(1)),
	    },
	    &operations);

	int         prompt_result = PAM_SUCCESS;
	char       *response      = nullptr;
	std::thread prompt_thread([&] -> void {
		prompt_result =
		    NativePromptConversationTestAccess::PromptInput(*conversation, prompt, &response, true);
	});

	std::array<char, 64> prompt_buffer{};
	const ssize_t        prompt_bytes = ReadWithTimeout(
	    master_fd.Get(), {.data = prompt_buffer.data(), .size = prompt_buffer.size()},
	    kPromptReadTimeoutMs);
	ok &= Expect(prompt_bytes > 0, "read EAGAIN abort test prompt is written to tty");

	ok &= Expect(WriteAll(master_fd.Get(), "s\n"), "read EAGAIN abort test makes tty readable");

	prompt_thread.join();

	ok &= Expect(prompt_result == PAM_CONV_ERR, "read EAGAIN abort test fails closed");
	ok &= Expect(response == nullptr, "read EAGAIN abort test returns no response");
	if (response != nullptr) {
		std::free(response);
	}
	return ok;
}

auto ExpectRestoreEintrRetriesAndRestores() -> bool {
	bool                    ok = true;
	ScopedFd                master_fd;
	ScopedFd                slave_fd;
	std::array<ScopedFd, 2> abort_pipe;

	ok &= Expect(OpenPtyPair(&master_fd, &slave_fd),
	             "restore EINTR retry test opens pseudo terminal");
	ok &= Expect(OpenPipe(&abort_pipe), "restore EINTR retry test creates abort pipe");
	if (!ok) {
		return false;
	}

	InputOperationContext operations{.restore_eintr_count = 1};
	auto const            conversation = CreateInputConversation(
	    {
	        .tty_fd         = std::move(slave_fd),
	        .abort_read_fd  = std::move(abort_pipe.at(0)),
	        .abort_write_fd = std::move(abort_pipe.at(1)),
	    },
	    &operations);
	return ExpectPromptInputReturnsPasswordAfterRetry(conversation.get(), master_fd.Get(),
	                                                  "restore EINTR retry test");
}

auto ExpectNativeMessageStyles() -> bool {
	bool                    ok = true;
	ScopedFd                master_fd;
	ScopedFd                slave_fd;
	std::array<ScopedFd, 2> abort_pipe;
	ok &= Expect(OpenPtyPair(&master_fd, &slave_fd), "message-style test opens pseudo terminal");
	ok &= Expect(OpenPipe(&abort_pipe), "message-style test creates abort pipe");
	if (!ok) {
		return false;
	}

	auto conversation = CreateConversation({
	    .tty_fd         = std::move(slave_fd),
	    .abort_read_fd  = std::move(abort_pipe.at(0)),
	    .abort_write_fd = std::move(abort_pipe.at(1)),
	});

	const struct pam_message  echo_on_message{.msg_style = PAM_PROMPT_ECHO_ON, .msg = "Login: "};
	const struct pam_message *echo_on_ptr      = &echo_on_message;
	struct pam_response      *echo_on_response = nullptr;
	int                       echo_on_result   = PAM_CONV_ERR;
	std::thread               echo_on_thread([&] -> void {
		echo_on_result = NativePromptConversationTestAccess::Dispatch(
		    1, &echo_on_ptr, &echo_on_response, conversation.get());
	});
	std::array<char, 64>      prompt_buffer{};
	ok &= Expect(ReadWithTimeout(master_fd.Get(),
	                             {.data = prompt_buffer.data(), .size = prompt_buffer.size()},
	                             kPromptReadTimeoutMs) > 0,
	             "message-style test writes echo-on prompt");
	constexpr std::string_view visible_input = "visible\n";
	ok &= Expect(WriteAll(master_fd.Get(), visible_input),
	             "message-style test writes echo-on response");
	echo_on_thread.join();
	ok &= Expect(echo_on_result == PAM_SUCCESS, "PAM_PROMPT_ECHO_ON dispatch succeeds");
	bool echo_on_response_valid = false;
	if (echo_on_response != nullptr) {
		auto const response_view = std::span(echo_on_response, std::size_t{1});
		echo_on_response_valid   = std::string(response_view.front().resp) == "visible";
		std::free(response_view.front().resp);
		std::free(echo_on_response);
	}
	ok &= Expect(echo_on_response_valid, "PAM_PROMPT_ECHO_ON returns visible input");

	for (const int style : {PAM_TEXT_INFO, PAM_ERROR_MSG}) {
		const struct pam_message  message{.msg_style = style, .msg = "notice"};
		const struct pam_message *message_ptr = &message;
		struct pam_response      *responses   = nullptr;
		const int result = NativePromptConversationTestAccess::Dispatch(1, &message_ptr, &responses,
		                                                                conversation.get());
		ok &= Expect(result == PAM_SUCCESS, "text and error message styles dispatch successfully");
		const bool empty_response =
		    responses != nullptr && std::span(responses, std::size_t{1}).front().resp == nullptr;
		ok &= Expect(empty_response, "text and error message styles return empty responses");
		ok &= Expect(ReadWithTimeout(master_fd.Get(),
		                             {.data = prompt_buffer.data(), .size = prompt_buffer.size()},
		                             kPromptReadTimeoutMs) > 0,
		             "text and error message styles write message lines");
		std::free(responses);
	}

	const struct pam_message  null_message{.msg_style = PAM_TEXT_INFO, .msg = nullptr};
	const struct pam_message *null_message_ptr = &null_message;
	struct pam_response      *null_responses   = nullptr;
	ok &= Expect(NativePromptConversationTestAccess::Dispatch(1, &null_message_ptr, &null_responses,
	                                                          conversation.get()) == PAM_SUCCESS,
	             "null message text is treated as empty text");
	ok &= Expect(ReadWithTimeout(master_fd.Get(),
	                             {.data = prompt_buffer.data(), .size = prompt_buffer.size()},
	                             kPromptReadTimeoutMs) > 0,
	             "null message text still writes newline");
	std::free(null_responses);

	const struct pam_message  unsupported_message{.msg_style = 99, .msg = "unsupported"};
	const struct pam_message *unsupported_ptr      = &unsupported_message;
	struct pam_response      *unsupported_response = nullptr;
	ok &=
	    Expect(NativePromptConversationTestAccess::Dispatch(
	               1, &unsupported_ptr, &unsupported_response, conversation.get()) == PAM_CONV_ERR,
		       "unsupported message style is rejected");
	ok &= Expect(unsupported_response == nullptr, "unsupported message style clears responses");

	auto const           invalid_tty          = CreateConversation({});
	struct pam_response *invalid_tty_response = nullptr;
	ok &=
	    Expect(NativePromptConversationTestAccess::Dispatch(
	               1, &null_message_ptr, &invalid_tty_response, invalid_tty.get()) == PAM_CONV_ERR,
		       "message dispatch rejects missing terminal");
	ok &= Expect(invalid_tty_response == nullptr,
	             "missing terminal message dispatch clears responses");

	const int closed_tty = NativePromptConversationTestAccess::TtyFd(*conversation);
	close(closed_tty);
	const struct pam_message  write_failure_message{.msg_style = PAM_TEXT_INFO, .msg = "failure"};
	const struct pam_message *write_failure_ptr      = &write_failure_message;
	struct pam_response      *write_failure_response = nullptr;
	ok &= Expect(NativePromptConversationTestAccess::Dispatch(1, &write_failure_ptr,
	                                                          &write_failure_response,
	                                                          conversation.get()) == PAM_CONV_ERR,
	             "message dispatch reports terminal write failure");
	ok &= Expect(write_failure_response == nullptr, "terminal write failure clears responses");
	return ok;
}

auto ExpectNativePromptInputEdges() -> bool {
	using howdy::pam::native_prompt_input::CharacterResult;
	using howdy::pam::native_prompt_input::ProcessCharacter;
	using howdy::pam::native_prompt_input::SensitiveBuffer;

	bool           ok = true;
	struct termios terminal{};
	terminal.c_cc[VINTR] = 0x18;  // stty intr ^X
	terminal.c_cc[VQUIT] = 0x1c;
	terminal.c_cc[VSUSP] = 0x1a;

	SensitiveBuffer password;
	bool            response_too_long = false;
	ok &= Expect(ProcessCharacter('x', terminal, password, response_too_long) ==
	                     CharacterResult::kKeepReading &&
	                 password.Size() == 1,
	             "ordinary character is appended to password");
	ok &= Expect(ProcessCharacter(127, terminal, password, response_too_long) ==
	                     CharacterResult::kKeepReading &&
	                 password.Size() == 0,
	             "DEL reaching the reader (erase is ^H) still erases");
	ok &= Expect(ProcessCharacter('\b', terminal, password, response_too_long) ==
	                     CharacterResult::kKeepReading &&
	                 password.Size() == 0,
	             "backspace on an empty password is ignored");
	for (const char byte : {'a', '\xc3', '\xa9'}) {
		(void)ProcessCharacter(byte, terminal, password, response_too_long);
	}
	(void)ProcessCharacter('\b', terminal, password, response_too_long);
	ok &= Expect(password.Size() == 2, "without IUTF8 backspace removes one byte");
	terminal.c_iflag = IUTF8;
	(void)ProcessCharacter('\xa9', terminal, password, response_too_long);
	(void)ProcessCharacter('\b', terminal, password, response_too_long);
	ok &= Expect(password.Size() == 1, "with IUTF8 backspace removes a whole UTF-8 character");
	ok &= Expect(ProcessCharacter(3, terminal, password, response_too_long) ==
	                     CharacterResult::kKeepReading &&
	                 password.Size() == 2,
	             "default ^C is data when the user remapped intr");
	for (const char signal_char : {'\x18', '\x1c', '\x1a', '\0'}) {
		ok &= Expect(ProcessCharacter(signal_char, terminal, password, response_too_long) ==
		                 CharacterResult::kAbort,
		             "configured intr, quit, susp, and NUL abort password input");
	}
	response_too_long = true;
	ok &= Expect(ProcessCharacter('y', terminal, password, response_too_long) ==
	                     CharacterResult::kKeepReading &&
	                 password.Size() == 2,
	             "characters after response overflow are drained");
	ok &= Expect(ProcessCharacter('\r', terminal, password, response_too_long) ==
	                 CharacterResult::kComplete,
	             "carriage return from raw-mode typeahead completes password input");
	ok &= Expect(ProcessCharacter('\n', terminal, password, response_too_long) ==
	                 CharacterResult::kComplete,
	             "newline completes password input");
	return ok;
}

namespace {
	struct PtyPromptCase {
		std::string_view input;
		bool             hide_input  = true;
		tcflag_t         extra_iflag = 0;
		tcflag_t         extra_lflag = 0;
		tcflag_t         clear_iflag = 0;
		tcflag_t         clear_lflag = 0;
		tcflag_t         clear_oflag = 0;
		bool             raw         = false;  // caller left the tty in cfmakeraw() mode
		std::string_view typeahead;            // typed before the prompt switched modes
		bool             abort_after_echo = false;
	};

	struct PtyPromptRun {
		int         result    = PAM_SUCCESS;
		bool        timed_out = false;
		std::string response;
		std::string output;    // what the terminal displayed
		std::string leftover;  // what the next reader gets after "next\n" is typed
	};

	auto ReadChunk(int fd, int timeout_ms) -> std::string {
		std::array<char, 256> buffer{};
		const ssize_t         count =
		    ReadWithTimeout(fd, {.data = buffer.data(), .size = buffer.size()}, timeout_ms);
		return count > 0 ? std::string(buffer.data(), static_cast<std::size_t>(count))
		                 : std::string();
	}

	// Runs one native prompt on a real pty so the kernel line discipline does the editing.
	auto RunPtyPrompt(const PtyPromptCase &test_case) -> std::optional<PtyPromptRun> {
		ScopedFd                master_fd;
		ScopedFd                slave_fd;
		std::array<ScopedFd, 2> abort_pipe;
		struct termios          terminal{};
		if (!OpenPtyPair(&master_fd, &slave_fd) || !OpenPipe(&abort_pipe) ||
		    tcgetattr(slave_fd.Get(), &terminal) != 0) {
			return std::nullopt;
		}
		if (test_case.raw) {
			cfmakeraw(&terminal);
		}
		terminal.c_iflag |= test_case.extra_iflag;
		terminal.c_lflag |= test_case.extra_lflag;
		terminal.c_iflag &= ~test_case.clear_iflag;
		terminal.c_lflag &= ~test_case.clear_lflag;
		terminal.c_oflag &= ~test_case.clear_oflag;
		if (tcsetattr(slave_fd.Get(), TCSANOW, &terminal) != 0) {
			return std::nullopt;
		}

		if (!WriteAll(master_fd.Get(), test_case.typeahead)) {
			return std::nullopt;
		}

		const int                slave_raw_fd = slave_fd.Get();
		auto const               conversation = CreateConversation({
		    .tty_fd         = std::move(slave_fd),
		    .abort_read_fd  = std::move(abort_pipe.at(0)),
		    .abort_write_fd = std::move(abort_pipe.at(1)),
		});
		const struct pam_message message{
		    .msg_style = test_case.hide_input ? PAM_PROMPT_ECHO_OFF : PAM_PROMPT_ECHO_ON,
		    .msg       = "P: ",
		};

		PtyPromptRun run;
		char        *response = nullptr;
		auto         result   = std::async(std::launch::async, [&] -> int {
			return NativePromptConversationTestAccess::PromptInput(*conversation, message,
			                                                       &response, test_case.hide_input);
		});

		run.output = ReadChunk(master_fd.Get(), kPromptReadTimeoutMs);
		(void)WriteAll(master_fd.Get(), test_case.input);
		if (test_case.abort_after_echo) {
			// The echo proves the kernel holds the partial line before the abort flushes it.
			while (!run.output.contains(test_case.input)) {
				const auto chunk = ReadChunk(master_fd.Get(), kPromptReadTimeoutMs);
				if (chunk.empty()) {
					break;
				}
				run.output += chunk;
			}
			conversation->RequestAbort();
		}
		if (result.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
			run.timed_out = true;
			conversation->RequestAbort();
		}
		run.result = result.get();
		for (auto chunk = ReadChunk(master_fd.Get(), 100); !chunk.empty();
		     chunk      = ReadChunk(master_fd.Get(), 100)) {
			run.output += chunk;
		}
		if (response != nullptr) {
			run.response = response;
			std::free(response);
		}

		(void)WriteAll(master_fd.Get(), "next\n");
		run.leftover = ReadChunk(slave_raw_fd, kPromptReadTimeoutMs);
		return run;
	}

	struct PtyPromptExpectation {
		int              result = PAM_SUCCESS;
		std::string_view response;
		std::string_view output;
	};

	auto ExpectPtyPrompt(const PtyPromptCase &test_case, const PtyPromptExpectation &expected,
	                     const std::string &message) -> bool {
		const auto maybe_run = RunPtyPrompt(test_case);
		if (!maybe_run.has_value()) {
			return Expect(false, message + ": sets up pty");
		}
		const PtyPromptRun &run = *maybe_run;
		bool                ok  = true;
		ok &= Expect(!run.timed_out, message + ": prompt finishes without an abort request");
		ok &= Expect(run.result == expected.result, message + ": result");
		ok &= Expect(run.response == expected.response, message + ": response");
		ok &= Expect(run.output == expected.output,
		             message + ": terminal output, got [" + run.output + "]");
		ok &= Expect(run.leftover == "next\n",
		             message + ": no input left for the next reader, got [" + run.leftover + "]");
		return ok;
	}
}  // namespace

auto ExpectKernelLineEditing() -> bool {
	constexpr std::string_view hidden_output = "P: \r\n";
	bool                       ok            = true;
	// ^U kill, then a UTF-8 character erased whole (IUTF8) with one DEL.
	ok &= ExpectPtyPrompt({.input = "wrong\025s\303\251\177ecret\n", .extra_iflag = IUTF8},
	                      {.response = "secret", .output = hidden_output}, "kill and UTF-8 erase");
	ok &= ExpectPtyPrompt({.input = "foo bar\027baz\n"},
	                      {.response = "foo baz", .output = hidden_output}, "word erase");
	ok &= ExpectPtyPrompt({.input = "a\026\025b\n"}, {.response = "b", .output = hidden_output},
	                      "literal next is disabled, so ^U still kills the line");
	ok &= ExpectPtyPrompt({.input = "ab\004c\n"}, {.response = "abc", .output = hidden_output},
	                      "EOF mid-line is not data");
	ok &= ExpectPtyPrompt({.input = "\004"}, {.result = PAM_CONV_ERR, .output = hidden_output},
	                      "EOF on empty line aborts");
	ok &= ExpectPtyPrompt({.input = "ab\032"}, {.result = PAM_CONV_ERR, .output = hidden_output},
	                      "suspend aborts without Enter");
	ok &= ExpectPtyPrompt({.input = "a\034b\n"}, {.result = PAM_CONV_ERR, .output = hidden_output},
	                      "quit aborts and discards the rest of the line");
	ok &= ExpectPtyPrompt({.input = "abc\r", .clear_iflag = ICRNL},
	                      {.response = "abc", .output = hidden_output},
	                      "Enter ends the line despite the caller's -icrnl");
	ok &= ExpectPtyPrompt({.input = "pwx\b\n"}, {.response = "pw", .output = hidden_output},
	                      "a Backspace byte that is not stty erase still erases");
	ok &= ExpectPtyPrompt({.input = "pv\177w zz\027\r", .raw = true},
	                      {.response = "pw ", .output = hidden_output},
	                      "Enter, erase and word erase work for a cfmakeraw() caller");
	ok &= ExpectPtyPrompt({.input = "a\003", .raw = true},
	                      {.result = PAM_CONV_ERR, .output = hidden_output},
	                      "intr aborts at once for a cfmakeraw() caller");
	ok &= ExpectPtyPrompt({.input = "a\032", .raw = true},
	                      {.result = PAM_CONV_ERR, .output = hidden_output},
	                      "susp aborts at once for a cfmakeraw() caller");
	ok &= ExpectPtyPrompt({.input = "", .raw = true, .typeahead = "pw\r"},
	                      {.response = "pw", .output = hidden_output},
	                      "raw-mode typeahead ending in carriage return completes");
	ok &= ExpectPtyPrompt({.input = "a\003", .clear_lflag = ISIG},
	                      {.result = PAM_CONV_ERR, .output = hidden_output},
	                      "intr aborts at once even when the caller had ISIG off");
	ok &= ExpectPtyPrompt({.input = "pw\n", .clear_oflag = ONLCR},
	                      {.response = "pw", .output = hidden_output},
	                      "prompt newline gets its carriage return despite the caller's -onlcr");
	ok &= ExpectPtyPrompt({.input = "pw\n", .extra_lflag = ECHONL},
	                      {.response = "pw", .output = hidden_output},
	                      "hidden prompt prints one newline even with ECHONL");
	ok &= ExpectPtyPrompt({.input = "vi\177s\n", .hide_input = false},
	                      {.response = "vs", .output = "P: vi\b \bs\r\n"},
	                      "echo-on prompt erases on screen and ends one line");
	ok &= ExpectPtyPrompt({.input = "part", .hide_input = false, .abort_after_echo = true},
	                      {.result = PAM_CONV_ERR, .output = "P: part\r\n"},
	                      "abort discards the half-typed line");
	return ok;
}

auto ExpectAbortDuringStoppedOutputDoesNotHang() -> bool {
	bool                    ok = true;
	ScopedFd                master_fd;
	ScopedFd                slave_fd;
	std::array<ScopedFd, 2> abort_pipe;
	ok &= Expect(OpenPtyPair(&master_fd, &slave_fd), "stopped output test opens pseudo terminal");
	ok &= Expect(OpenPipe(&abort_pipe), "stopped output test creates abort pipe");
	if (!ok) {
		return false;
	}

	// Same effect as the user pressing ^S with IXON: writes to the tty stall.
	const int slave_raw_fd = slave_fd.Get();
	ok &= Expect(tcflow(slave_raw_fd, TCOOFF) == 0, "stopped output test stops tty output");
	// Production opens PAM_TTY with O_NONBLOCK (OpenTtyFd).
	ok &= Expect(
	    fcntl(slave_raw_fd, F_SETFL,
		      howdy::native::CombineBitsAs<int>(fcntl(slave_raw_fd, F_GETFL), O_NONBLOCK)) == 0,
	    "stopped output test makes the tty non-blocking like production");
	auto const               conversation = CreateConversation({
	    .tty_fd         = std::move(slave_fd),
	    .abort_read_fd  = std::move(abort_pipe.at(0)),
	    .abort_write_fd = std::move(abort_pipe.at(1)),
	});
	const struct pam_message message{.msg_style = PAM_PROMPT_ECHO_OFF, .msg = "P: "};
	char                    *response = nullptr;
	auto                     result   = std::async(std::launch::async, [&] -> int {
		return NativePromptConversationTestAccess::PromptInput(*conversation, message, &response,
		                                                       true);
	});

	ok &= Expect(result.wait_for(std::chrono::milliseconds(300)) == std::future_status::timeout,
	             "prompt write waits while output is stopped");
	conversation->RequestAbort();
	const bool finished = result.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
	ok &= Expect(finished, "abort ends the prompt even while output is stopped");
	(void)tcflow(slave_raw_fd, TCOON);
	ok &= Expect(result.get() == PAM_CONV_ERR, "aborted stopped-output prompt fails closed");
	ok &= Expect(response == nullptr, "aborted stopped-output prompt returns no response");
	std::free(response);
	return ok;
}

auto ExpectOversizedPromptFailsClosed() -> bool {
	bool                    ok = true;
	ScopedFd                master_fd;
	ScopedFd                slave_fd;
	std::array<ScopedFd, 2> abort_pipe;

	ok &= Expect(OpenPtyPair(&master_fd, &slave_fd), "oversized prompt test opens pseudo terminal");
	ok &= Expect(OpenPipe(&abort_pipe), "oversized prompt test creates abort pipe");
	if (!ok) {
		return false;
	}

	const struct pam_message prompt = {
	    .msg_style = PAM_PROMPT_ECHO_OFF,
	    .msg       = "Password: ",
	};
	const int slave_raw_fd = slave_fd.Get();
	auto      conversation = CreateConversation({
	    .tty_fd         = std::move(slave_fd),
	    .abort_read_fd  = std::move(abort_pipe.at(0)),
	    .abort_write_fd = std::move(abort_pipe.at(1)),
	});

	int         prompt_result = PAM_SUCCESS;
	char       *response      = nullptr;
	std::thread prompt_thread([&] -> void {
		prompt_result =
		    NativePromptConversationTestAccess::PromptInput(*conversation, prompt, &response, true);
	});

	std::array<char, 64> prompt_buffer{};
	const ssize_t        prompt_bytes = ReadWithTimeout(
	    master_fd.Get(), {.data = prompt_buffer.data(), .size = prompt_buffer.size()},
	    kPromptReadTimeoutMs);
	ok &= Expect(prompt_bytes > 0, "oversized prompt test writes prompt to tty");

	const std::string oversized_response = std::string(514, 'x') + "\n";
	ok &= Expect(WriteAll(master_fd.Get(), oversized_response),
	             "oversized prompt test writes over-limit response");
	prompt_thread.join();

	ok &=
	    Expect(prompt_result == PAM_CONV_ERR, "oversized prompt test rejects over-limit response");
	ok &= Expect(response == nullptr, "oversized prompt test returns no response");
	struct termios restored_termios{};
	ok &= Expect(tcgetattr(slave_raw_fd, &restored_termios) == 0,
	             "oversized prompt test can inspect restored terminal");
	ok &= Expect(howdy::native::HasAllBits(restored_termios.c_lflag, ICANON, ECHO, ISIG),
	             "oversized prompt test restores terminal flags");
	if (response != nullptr) {
		std::free(response);
	}
	return ok;
}

auto ExpectRestoreFailureFailsClosed() -> bool {
	bool                    ok = true;
	ScopedFd                master_fd;
	ScopedFd                slave_fd;
	std::array<ScopedFd, 2> abort_pipe;

	ok &= Expect(OpenPtyPair(&master_fd, &slave_fd), "restore failure test opens pseudo terminal");
	ok &= Expect(OpenPipe(&abort_pipe), "restore failure test creates abort pipe");
	if (!ok) {
		return false;
	}

	const struct pam_message prompt = {
	    .msg_style = PAM_PROMPT_ECHO_OFF,
	    .msg       = "Password: ",
	};
	InputOperationContext operations{.restore_failure = true};
	auto                  conversation = CreateInputConversation(
	    {
	        .tty_fd         = std::move(slave_fd),
	        .abort_read_fd  = std::move(abort_pipe.at(0)),
	        .abort_write_fd = std::move(abort_pipe.at(1)),
	    },
	    &operations);

	int         prompt_result = PAM_SUCCESS;
	char       *response      = nullptr;
	std::thread prompt_thread([&] -> void {
		prompt_result =
		    NativePromptConversationTestAccess::PromptInput(*conversation, prompt, &response, true);
	});

	std::array<char, 64> prompt_buffer{};
	const ssize_t        prompt_bytes = ReadWithTimeout(
	    master_fd.Get(), {.data = prompt_buffer.data(), .size = prompt_buffer.size()},
	    kPromptReadTimeoutMs);
	ok &= Expect(prompt_bytes > 0, "restore failure test writes prompt to tty");
	constexpr std::array<char, 7> password{'s', 'e', 'c', 'r', 'e', 't', '\n'};
	ok &= Expect(WriteAll(master_fd.Get(), std::string_view(password.data(), password.size())),
	             "restore failure test writes password response");
	prompt_thread.join();

	ok &= Expect(prompt_result == PAM_CONV_ERR,
	             "restore failure test returns conversation error after terminal restore error");
	ok &= Expect(NativePromptConversationTestAccess::TerminalRestoreFailed(*conversation),
	             "restore failure test records terminal restore failure");
	ok &= Expect(response == nullptr, "restore failure test returns no response");
	if (response != nullptr) {
		std::free(response);
	}
	return ok;
}

auto ExpectPromptExceptionRestoresTerminalAndRecordsRestoreFailure() -> bool {
	bool ok = true;

	for (const bool restore_failure : {false, true}) {
		ScopedFd                master_fd;
		ScopedFd                slave_fd;
		std::array<ScopedFd, 2> abort_pipe;

		ok &= Expect(OpenPtyPair(&master_fd, &slave_fd), "exception restore test opens pty");
		ok &= Expect(OpenPipe(&abort_pipe), "exception restore test creates abort pipe");
		if (!ok) {
			return false;
		}

		const struct pam_message  prompt{.msg_style = PAM_PROMPT_ECHO_OFF, .msg = "Password: "};
		const struct pam_message *prompt_ptr   = &prompt;
		const int                 slave_raw_fd = slave_fd.Get();
		InputOperationContext     operations{
		    .throw_on_poll   = true,
		    .restore_failure = restore_failure,
		};
		auto const conversation = CreateInputConversation(
		    {
		        .tty_fd         = std::move(slave_fd),
		        .abort_read_fd  = std::move(abort_pipe.at(0)),
		        .abort_write_fd = std::move(abort_pipe.at(1)),
		    },
		    &operations);

		auto     *responses = howdy::test::PamResponseSentinelForTest();
		const int result = NativePromptConversationTestAccess::Dispatch(1, &prompt_ptr, &responses,
		                                                                conversation.get());
		ok &= Expect(result == PAM_CONV_ERR, "exception during prompt fails closed");
		ok &= Expect(responses == nullptr, "exception during prompt clears responses");
		ok &= Expect(NativePromptConversationTestAccess::TerminalRestoreFailed(*conversation) ==
		                 restore_failure,
		             "exception during prompt records terminal restore outcome");

		if (!restore_failure) {
			struct termios restored_termios{};
			ok &= Expect(tcgetattr(slave_raw_fd, &restored_termios) == 0,
			             "exception restore test reads restored terminal state");
			ok &= Expect(howdy::native::HasAllBits(restored_termios.c_lflag, ICANON, ECHO, ISIG),
			             "exception during prompt restores terminal flags");
		}
	}

	return ok;
}
