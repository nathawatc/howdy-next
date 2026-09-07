#include "prompt/native_prompt_input.hpp"
#include "prompt/native_prompt_test_support.hpp"
#include "test_support.hpp"

#include <array>
#include <chrono>
#include <cstdlib>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

using howdy::test::expect;

auto ExpectCtrlCAbortsPromptAndRestoresTerminal() -> bool;
auto ExpectAbortRequestUnblocksWithoutPipeWakeup() -> bool;
auto ExpectPtyHangupAbortsPrompt() -> bool;
auto ExpectPollEintrWithoutAbortDoesNotAbortPrompt() -> bool;
auto ExpectPollEintrWithAbortFailsClosed() -> bool;
auto ExpectReadEintrRetriesAndAcceptsInput() -> bool;
auto ExpectReadEintrWithAbortFailsClosed() -> bool;
auto ExpectReadZeroRetriesAndAcceptsInput() -> bool;
auto ExpectRestoreEintrRetriesAndRestores() -> bool;
auto ExpectNativeMessageStyles() -> bool;
auto ExpectNativePromptInputEdges() -> bool;
auto ExpectOversizedPromptFailsClosed() -> bool;
auto ExpectRestoreFailureFailsClosed() -> bool;

namespace {
	struct InputOperationContext {
		NativePromptConversation *conversation        = nullptr;
		int                       poll_eintr_count    = 0;
		int                       read_eintr_count    = 0;
		int                       read_zero_count     = 0;
		int                       restore_eintr_count = 0;
		bool                      abort_on_poll       = false;
		bool                      abort_on_read       = false;
		bool                      restore_failure     = false;
	};

	auto InjectedPoll(void *context, struct pollfd *fds, nfds_t count, int timeout) -> int {
		auto &operations = *static_cast<InputOperationContext *>(context);
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
		    descriptors, operations == nullptr
		                     ? NativePromptConversationTestAccess::Operations{}
		                     : NativePromptConversationTestAccess::Operations{
		                           .context          = operations,
		                           .poll_prompt      = InjectedPoll,
		                           .read_prompt      = InjectedRead,
		                           .restore_terminal = InjectedRestore,
		                           .post_message     = NativePromptInputInjectedPostMessage,
		                       });
		if (operations != nullptr) {
			operations->conversation = conversation.get();
		}
		return conversation;
	}

	auto WriteAll(int fd, const char *data, std::size_t size) -> bool {
		std::size_t written = 0;
		while (written < size) {
			const ssize_t result = write(fd, data + written, size - written);
			if (result < 0 && errno == EINTR) {
				continue;
			}
			if (result <= 0) {
				return false;
			}
			written += static_cast<std::size_t>(result);
		}
		return true;
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
		ok &= expect(prompt_bytes > 0, message + ": prompt is written to tty");

		constexpr std::array<char, 7> password{'s', 'e', 'c', 'r', 'e', 't', '\n'};
		ok &= expect(write(master_fd, password.data(), password.size()) ==
		                 static_cast<ssize_t>(password.size()),
		             message + ": writes password response");

		prompt_thread.join();

		ok &= expect(prompt_result == PAM_SUCCESS, message + ": prompt succeeds after EINTR");
		ok &= expect(response != nullptr, message + ": prompt returns response");
		if (response != nullptr) {
			ok &= expect(std::string(response) == "secret", message + ": response matches input");
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

	ok &= expect(OpenPtyPair(&master_fd, &slave_fd), "opens pseudo terminal");
	ok &= expect(OpenPipe(&abort_pipe), "creates abort pipe");
	if (!ok) {
		return false;
	}

	const struct pam_message message = {
	    .msg_style = PAM_PROMPT_ECHO_OFF,
	    .msg       = "Password: ",
	};

	const int slave_raw_fd = slave_fd.get();
	auto      conversation = CreateConversation({.tty_fd         = slave_fd.release(),
	                                             .abort_read_fd  = abort_pipe[0].release(),
	                                             .abort_write_fd = abort_pipe[1].release()});

	int         prompt_result = PAM_SUCCESS;
	char       *response      = nullptr;
	std::thread prompt_thread([&] -> void {
		prompt_result = NativePromptConversationTestAccess::PromptInput(*conversation, message,
		                                                                &response, true);
	});

	std::array<char, 64> prompt_buffer{};
	const ssize_t        prompt_bytes = ReadWithTimeout(
	    master_fd.get(), {.data = prompt_buffer.data(), .size = prompt_buffer.size()},
	    kPromptReadTimeoutMs);
	ok &= expect(prompt_bytes > 0, "prompt is written to tty");

	constexpr char ctrl_c = 3;
	ok &= expect(write(master_fd.get(), &ctrl_c, 1) == 1, "writes Ctrl-C byte to pseudo terminal");

	prompt_thread.join();

	ok &= expect(prompt_result == PAM_CONV_ERR, "Ctrl-C byte aborts the native prompt");
	ok &= expect(!NativePromptConversationTestAccess::TerminalRestoreFailed(*conversation),
	             "successful terminal restoration keeps cancellation non-fatal");
	ok &= expect(response == nullptr, "aborted prompt does not return a response");

	struct termios restored_termios{};
	ok &= expect(tcgetattr(slave_raw_fd, &restored_termios) == 0,
	             "terminal state remains readable after abort");
	ok &= expect((restored_termios.c_lflag & ICANON) != 0, "canonical mode restored after abort");
	ok &= expect((restored_termios.c_lflag & ECHO) != 0, "echo restored after abort");
	ok &= expect((restored_termios.c_lflag & ISIG) != 0, "signal generation restored after abort");

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
	ok &= expect(OpenPtyPair(&master_fd, &slave_fd), "abort wake test opens pseudo terminal");
	ok &= expect(OpenPipe(&abort_pipe), "abort wake test creates abort pipe");
	if (!ok) {
		return false;
	}

	const struct pam_message message = {
	    .msg_style = PAM_PROMPT_ECHO_OFF,
	    .msg       = "Password: ",
	};

	InputOperationContext operations{.restore_failure = true};
	auto                  conversation = std::shared_ptr<NativePromptConversation>(
	    CreateInputConversation({.tty_fd         = slave_fd.release(),
	                             .abort_read_fd  = abort_pipe[0].release(),
	                             .abort_write_fd = abort_pipe[1].release()},
	                            &operations));
	NativePromptConversationTestAccess::CloseAbortWriteFd(*conversation);

	auto         response       = std::make_shared<char *>(nullptr);
	auto         result_promise = std::make_shared<std::promise<int>>();
	auto         result_future  = result_promise->get_future();
	std::jthread prompt_thread([conversation, message, response, result_promise] -> void {
		result_promise->set_value(NativePromptConversationTestAccess::PromptInput(
		    *conversation, message, response.get(), true));
	});

	std::array<char, 64> prompt_buffer{};
	const ssize_t        prompt_bytes = ReadWithTimeout(
	    master_fd.get(), {.data = prompt_buffer.data(), .size = prompt_buffer.size()},
	    kPromptReadTimeoutMs);
	ok &= expect(prompt_bytes > 0, "abort wake test prompt is written to tty");

	conversation->RequestAbort();
	if (result_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
		master_fd.reset();
		conversation->RequestAbort();
	}
	if (result_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
		(void)expect(false, "abort wake test prompt thread stops before timeout");
		_exit(EXIT_FAILURE);
	}
	conversation->RequestAbort();
	prompt_thread.join();

	const int prompt_result = result_future.get();
	ok &= expect(prompt_result == PAM_CONV_ERR,
	             "abort wake test terminal restore failure returns conversation error");
	ok &= expect(NativePromptConversationTestAccess::TerminalRestoreFailed(*conversation),
	             "abort wake test records terminal restore failure");
	ok &= expect(*response == nullptr, "abort wake test returns no response");
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
	ok &= expect(OpenPtyPair(&master_fd, &slave_fd), "hangup test opens pseudo terminal");
	ok &= expect(OpenPipe(&abort_pipe), "hangup test creates abort pipe");
	if (!ok) {
		return false;
	}

	const struct pam_message message = {
	    .msg_style = PAM_PROMPT_ECHO_OFF,
	    .msg       = "Password: ",
	};

	auto conversation = std::shared_ptr<NativePromptConversation>(
	    CreateConversation({.tty_fd         = slave_fd.release(),
	                        .abort_read_fd  = abort_pipe[0].release(),
	                        .abort_write_fd = abort_pipe[1].release()}));

	auto         response       = std::make_shared<char *>(nullptr);
	auto         result_promise = std::make_shared<std::promise<int>>();
	auto         result_future  = result_promise->get_future();
	std::jthread prompt_thread([conversation, message, response, result_promise] -> void {
		result_promise->set_value(NativePromptConversationTestAccess::PromptInput(
		    *conversation, message, response.get(), true));
	});

	std::array<char, 64> prompt_buffer{};
	const ssize_t        prompt_bytes = ReadWithTimeout(
	    master_fd.get(), {.data = prompt_buffer.data(), .size = prompt_buffer.size()},
	    kPromptReadTimeoutMs);
	ok &= expect(prompt_bytes > 0, "hangup test prompt is written to tty");

	master_fd.reset();
	if (result_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
		conversation->RequestAbort();
	}
	if (result_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
		(void)expect(false, "PTY hangup prompt thread stops before timeout");
		_exit(EXIT_FAILURE);
	}
	conversation->RequestAbort();
	prompt_thread.join();

	const int prompt_result = result_future.get();
	ok &= expect(prompt_result == PAM_CONV_ERR, "PTY hangup aborts prompt");
	ok &= expect(*response == nullptr, "PTY hangup returns no response");
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

	ok &= expect(OpenPtyPair(&master_fd, &slave_fd), "poll EINTR retry test opens pseudo terminal");
	ok &= expect(OpenPipe(&abort_pipe), "poll EINTR retry test creates abort pipe");
	if (!ok) {
		return false;
	}

	InputOperationContext operations{.poll_eintr_count = 1};
	auto conversation = CreateInputConversation({.tty_fd         = slave_fd.release(),
	                                             .abort_read_fd  = abort_pipe[0].release(),
	                                             .abort_write_fd = abort_pipe[1].release()},
	                                            &operations);
	return ExpectPromptInputReturnsPasswordAfterRetry(conversation.get(), master_fd.get(),
	                                                  "poll EINTR retry test");
}

auto ExpectPollEintrWithAbortFailsClosed() -> bool {
	bool                    ok = true;
	ScopedFd                master_fd;
	ScopedFd                slave_fd;
	std::array<ScopedFd, 2> abort_pipe;

	ok &= expect(OpenPtyPair(&master_fd, &slave_fd), "poll EINTR abort test opens pseudo terminal");
	ok &= expect(OpenPipe(&abort_pipe), "poll EINTR abort test creates abort pipe");
	if (!ok) {
		return false;
	}

	const struct pam_message prompt = {
	    .msg_style = PAM_PROMPT_ECHO_OFF,
	    .msg       = "Password: ",
	};

	InputOperationContext operations{.poll_eintr_count = 1, .abort_on_poll = true};
	auto conversation = CreateInputConversation({.tty_fd         = slave_fd.release(),
	                                             .abort_read_fd  = abort_pipe[0].release(),
	                                             .abort_write_fd = abort_pipe[1].release()},
	                                            &operations);

	int         prompt_result = PAM_SUCCESS;
	char       *response      = nullptr;
	std::thread prompt_thread([&] -> void {
		prompt_result =
		    NativePromptConversationTestAccess::PromptInput(*conversation, prompt, &response, true);
	});

	std::array<char, 64> prompt_buffer{};
	const ssize_t        prompt_bytes = ReadWithTimeout(
	    master_fd.get(), {.data = prompt_buffer.data(), .size = prompt_buffer.size()},
	    kPromptReadTimeoutMs);
	ok &= expect(prompt_bytes > 0, "poll EINTR abort test prompt is written to tty");

	prompt_thread.join();

	ok &= expect(prompt_result == PAM_CONV_ERR, "poll EINTR abort test fails closed");
	ok &= expect(response == nullptr, "poll EINTR abort test returns no response");
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

	ok &= expect(OpenPtyPair(&master_fd, &slave_fd), "read EINTR retry test opens pseudo terminal");
	ok &= expect(OpenPipe(&abort_pipe), "read EINTR retry test creates abort pipe");
	if (!ok) {
		return false;
	}

	InputOperationContext operations{.read_eintr_count = 1};
	auto conversation = CreateInputConversation({.tty_fd         = slave_fd.release(),
	                                             .abort_read_fd  = abort_pipe[0].release(),
	                                             .abort_write_fd = abort_pipe[1].release()},
	                                            &operations);
	return ExpectPromptInputReturnsPasswordAfterRetry(conversation.get(), master_fd.get(),
	                                                  "read EINTR retry test");
}

auto ExpectReadEintrWithAbortFailsClosed() -> bool {
	bool                    ok = true;
	ScopedFd                master_fd;
	ScopedFd                slave_fd;
	std::array<ScopedFd, 2> abort_pipe;

	ok &= expect(OpenPtyPair(&master_fd, &slave_fd), "read EINTR abort test opens pseudo terminal");
	ok &= expect(OpenPipe(&abort_pipe), "read EINTR abort test creates abort pipe");
	if (!ok) {
		return false;
	}

	const struct pam_message prompt = {
	    .msg_style = PAM_PROMPT_ECHO_OFF,
	    .msg       = "Password: ",
	};

	InputOperationContext operations{.read_eintr_count = 1, .abort_on_read = true};
	auto conversation = CreateInputConversation({.tty_fd         = slave_fd.release(),
	                                             .abort_read_fd  = abort_pipe[0].release(),
	                                             .abort_write_fd = abort_pipe[1].release()},
	                                            &operations);

	int         prompt_result = PAM_SUCCESS;
	char       *response      = nullptr;
	std::thread prompt_thread([&] -> void {
		prompt_result =
		    NativePromptConversationTestAccess::PromptInput(*conversation, prompt, &response, true);
	});

	std::array<char, 64> prompt_buffer{};
	const ssize_t        prompt_bytes = ReadWithTimeout(
	    master_fd.get(), {.data = prompt_buffer.data(), .size = prompt_buffer.size()},
	    kPromptReadTimeoutMs);
	ok &= expect(prompt_bytes > 0, "read EINTR abort test prompt is written to tty");

	constexpr char password = 's';
	ok &= expect(write(master_fd.get(), &password, 1) == 1,
	             "read EINTR abort test makes tty readable");

	prompt_thread.join();

	ok &= expect(prompt_result == PAM_CONV_ERR, "read EINTR abort test fails closed");
	ok &= expect(response == nullptr, "read EINTR abort test returns no response");
	if (response != nullptr) {
		std::free(response);
	}
	return ok;
}

auto ExpectReadZeroRetriesAndAcceptsInput() -> bool {
	bool                    ok = true;
	ScopedFd                master_fd;
	ScopedFd                slave_fd;
	std::array<ScopedFd, 2> abort_pipe;

	ok &= expect(OpenPtyPair(&master_fd, &slave_fd), "read zero retry test opens pseudo terminal");
	ok &= expect(OpenPipe(&abort_pipe), "read zero retry test creates abort pipe");
	if (!ok) {
		return false;
	}

	InputOperationContext operations{.read_zero_count = 1};
	auto conversation = CreateInputConversation({.tty_fd         = slave_fd.release(),
	                                             .abort_read_fd  = abort_pipe[0].release(),
	                                             .abort_write_fd = abort_pipe[1].release()},
	                                            &operations);
	return ExpectPromptInputReturnsPasswordAfterRetry(conversation.get(), master_fd.get(),
	                                                  "read zero retry test");
}

auto ExpectRestoreEintrRetriesAndRestores() -> bool {
	bool                    ok = true;
	ScopedFd                master_fd;
	ScopedFd                slave_fd;
	std::array<ScopedFd, 2> abort_pipe;

	ok &= expect(OpenPtyPair(&master_fd, &slave_fd),
	             "restore EINTR retry test opens pseudo terminal");
	ok &= expect(OpenPipe(&abort_pipe), "restore EINTR retry test creates abort pipe");
	if (!ok) {
		return false;
	}

	InputOperationContext operations{.restore_eintr_count = 1};
	auto conversation = CreateInputConversation({.tty_fd         = slave_fd.release(),
	                                             .abort_read_fd  = abort_pipe[0].release(),
	                                             .abort_write_fd = abort_pipe[1].release()},
	                                            &operations);
	return ExpectPromptInputReturnsPasswordAfterRetry(conversation.get(), master_fd.get(),
	                                                  "restore EINTR retry test");
}

auto ExpectNativeMessageStyles() -> bool {
	bool                    ok = true;
	ScopedFd                master_fd;
	ScopedFd                slave_fd;
	std::array<ScopedFd, 2> abort_pipe;
	ok &= expect(OpenPtyPair(&master_fd, &slave_fd), "message-style test opens pseudo terminal");
	ok &= expect(OpenPipe(&abort_pipe), "message-style test creates abort pipe");
	if (!ok) {
		return false;
	}

	auto conversation = CreateConversation({.tty_fd         = slave_fd.release(),
	                                        .abort_read_fd  = abort_pipe[0].release(),
	                                        .abort_write_fd = abort_pipe[1].release()});

	const struct pam_message  echo_on_message{.msg_style = PAM_PROMPT_ECHO_ON, .msg = "Login: "};
	const struct pam_message *echo_on_ptr      = &echo_on_message;
	struct pam_response      *echo_on_response = nullptr;
	int                       echo_on_result   = PAM_CONV_ERR;
	std::thread               echo_on_thread([&] -> void {
		echo_on_result = NativePromptConversationTestAccess::Dispatch(
		    1, &echo_on_ptr, &echo_on_response, conversation.get());
	});
	std::array<char, 64>      prompt_buffer{};
	ok &= expect(ReadWithTimeout(master_fd.get(),
	                             {.data = prompt_buffer.data(), .size = prompt_buffer.size()},
	                             kPromptReadTimeoutMs) > 0,
	             "message-style test writes echo-on prompt");
	constexpr std::string_view visible_input = "visible\n";
	ok &= expect(WriteAll(master_fd.get(), visible_input.data(), visible_input.size()),
	             "message-style test writes echo-on response");
	echo_on_thread.join();
	ok &= expect(echo_on_result == PAM_SUCCESS, "PAM_PROMPT_ECHO_ON dispatch succeeds");
	ok &= expect(echo_on_response != nullptr && std::string(echo_on_response[0].resp) == "visible",
	             "PAM_PROMPT_ECHO_ON returns visible input");
	if (echo_on_response != nullptr) {
		std::free(echo_on_response[0].resp);
		std::free(echo_on_response);
	}

	for (const int style : {PAM_TEXT_INFO, PAM_ERROR_MSG}) {
		const struct pam_message  message{.msg_style = style, .msg = "notice"};
		const struct pam_message *message_ptr = &message;
		struct pam_response      *responses   = nullptr;
		const int result = NativePromptConversationTestAccess::Dispatch(1, &message_ptr, &responses,
		                                                                conversation.get());
		ok &= expect(result == PAM_SUCCESS, "text and error message styles dispatch successfully");
		ok &= expect(responses != nullptr && responses[0].resp == nullptr,
		             "text and error message styles return empty responses");
		ok &= expect(ReadWithTimeout(master_fd.get(),
		                             {.data = prompt_buffer.data(), .size = prompt_buffer.size()},
		                             kPromptReadTimeoutMs) > 0,
		             "text and error message styles write message lines");
		std::free(responses);
	}

	const struct pam_message  null_message{.msg_style = PAM_TEXT_INFO, .msg = nullptr};
	const struct pam_message *null_message_ptr = &null_message;
	struct pam_response      *null_responses   = nullptr;
	ok &= expect(NativePromptConversationTestAccess::Dispatch(1, &null_message_ptr, &null_responses,
	                                                          conversation.get()) == PAM_SUCCESS,
	             "null message text is treated as empty text");
	ok &= expect(ReadWithTimeout(master_fd.get(),
	                             {.data = prompt_buffer.data(), .size = prompt_buffer.size()},
	                             kPromptReadTimeoutMs) > 0,
	             "null message text still writes newline");
	std::free(null_responses);

	const struct pam_message  unsupported_message{.msg_style = 99, .msg = "unsupported"};
	const struct pam_message *unsupported_ptr      = &unsupported_message;
	struct pam_response      *unsupported_response = nullptr;
	ok &=
	    expect(NativePromptConversationTestAccess::Dispatch(
	               1, &unsupported_ptr, &unsupported_response, conversation.get()) == PAM_CONV_ERR,
	           "unsupported message style is rejected");
	ok &= expect(unsupported_response == nullptr, "unsupported message style clears responses");

	auto                 invalid_tty          = CreateConversation({});
	struct pam_response *invalid_tty_response = nullptr;
	ok &=
	    expect(NativePromptConversationTestAccess::Dispatch(
	               1, &null_message_ptr, &invalid_tty_response, invalid_tty.get()) == PAM_CONV_ERR,
	           "message dispatch rejects missing terminal");
	ok &= expect(invalid_tty_response == nullptr,
	             "missing terminal message dispatch clears responses");

	const int closed_tty = NativePromptConversationTestAccess::TtyFd(*conversation);
	close(closed_tty);
	const struct pam_message  write_failure_message{.msg_style = PAM_TEXT_INFO, .msg = "failure"};
	const struct pam_message *write_failure_ptr      = &write_failure_message;
	struct pam_response      *write_failure_response = nullptr;
	ok &= expect(NativePromptConversationTestAccess::Dispatch(1, &write_failure_ptr,
	                                                          &write_failure_response,
	                                                          conversation.get()) == PAM_CONV_ERR,
	             "message dispatch reports terminal write failure");
	ok &= expect(write_failure_response == nullptr, "terminal write failure clears responses");
	return ok;
}

auto ExpectNativePromptInputEdges() -> bool {
	using howdy::pam::native_prompt_input::CharacterResult;
	using howdy::pam::native_prompt_input::SensitiveBuffer;

	bool            ok = true;
	SensitiveBuffer password;
	bool            response_too_long = false;
	ok &= expect(howdy::pam::native_prompt_input::ProcessCharacter(
	                 '\b', password, response_too_long) == CharacterResult::kEepReading &&
	                 password.Empty(),
	             "backspace on empty password is ignored");
	ok &= expect(howdy::pam::native_prompt_input::ProcessCharacter(
	                 'x', password, response_too_long) == CharacterResult::kEepReading &&
	                 password.Size() == 1,
	             "ordinary character is appended to password");
	ok &= expect(howdy::pam::native_prompt_input::ProcessCharacter(
	                 127, password, response_too_long) == CharacterResult::kEepReading &&
	                 password.Empty(),
	             "delete removes last password character");
	response_too_long = true;
	ok &= expect(howdy::pam::native_prompt_input::ProcessCharacter(
	                 'y', password, response_too_long) == CharacterResult::kEepReading &&
	                 password.Empty(),
	             "characters after response overflow are drained");
	ok &= expect(howdy::pam::native_prompt_input::ProcessCharacter(
	                 '\r', password, response_too_long) == CharacterResult::kComplete,
	             "carriage return completes password input");
	ok &= expect(howdy::pam::native_prompt_input::ProcessCharacter(
	                 3, password, response_too_long) == CharacterResult::kAbort,
	             "Ctrl-C aborts password input");
	return ok;
}

auto ExpectOversizedPromptFailsClosed() -> bool {
	bool                    ok = true;
	ScopedFd                master_fd;
	ScopedFd                slave_fd;
	std::array<ScopedFd, 2> abort_pipe;

	ok &= expect(OpenPtyPair(&master_fd, &slave_fd), "oversized prompt test opens pseudo terminal");
	ok &= expect(OpenPipe(&abort_pipe), "oversized prompt test creates abort pipe");
	if (!ok) {
		return false;
	}

	const struct pam_message prompt = {
	    .msg_style = PAM_PROMPT_ECHO_OFF,
	    .msg       = "Password: ",
	};
	const int slave_raw_fd = slave_fd.get();
	auto      conversation = CreateConversation({.tty_fd         = slave_fd.release(),
	                                             .abort_read_fd  = abort_pipe[0].release(),
	                                             .abort_write_fd = abort_pipe[1].release()});

	int         prompt_result = PAM_SUCCESS;
	char       *response      = nullptr;
	std::thread prompt_thread([&] -> void {
		prompt_result =
		    NativePromptConversationTestAccess::PromptInput(*conversation, prompt, &response, true);
	});

	std::array<char, 64> prompt_buffer{};
	const ssize_t        prompt_bytes = ReadWithTimeout(
	    master_fd.get(), {.data = prompt_buffer.data(), .size = prompt_buffer.size()},
	    kPromptReadTimeoutMs);
	ok &= expect(prompt_bytes > 0, "oversized prompt test writes prompt to tty");

	const std::string oversized_response = std::string(514, 'x') + "\n";
	ok &= expect(WriteAll(master_fd.get(), oversized_response.data(), oversized_response.size()),
	             "oversized prompt test writes over-limit response");
	prompt_thread.join();

	ok &=
	    expect(prompt_result == PAM_CONV_ERR, "oversized prompt test rejects over-limit response");
	ok &= expect(response == nullptr, "oversized prompt test returns no response");
	struct termios restored_termios{};
	ok &= expect(tcgetattr(slave_raw_fd, &restored_termios) == 0,
	             "oversized prompt test can inspect restored terminal");
	ok &= expect((restored_termios.c_lflag & (ICANON | ECHO | ISIG)) == (ICANON | ECHO | ISIG),
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

	ok &= expect(OpenPtyPair(&master_fd, &slave_fd), "restore failure test opens pseudo terminal");
	ok &= expect(OpenPipe(&abort_pipe), "restore failure test creates abort pipe");
	if (!ok) {
		return false;
	}

	const struct pam_message prompt = {
	    .msg_style = PAM_PROMPT_ECHO_OFF,
	    .msg       = "Password: ",
	};
	InputOperationContext operations{.restore_failure = true};
	auto conversation = CreateInputConversation({.tty_fd         = slave_fd.release(),
	                                             .abort_read_fd  = abort_pipe[0].release(),
	                                             .abort_write_fd = abort_pipe[1].release()},
	                                            &operations);

	int         prompt_result = PAM_SUCCESS;
	char       *response      = nullptr;
	std::thread prompt_thread([&] -> void {
		prompt_result =
		    NativePromptConversationTestAccess::PromptInput(*conversation, prompt, &response, true);
	});

	std::array<char, 64> prompt_buffer{};
	const ssize_t        prompt_bytes = ReadWithTimeout(
	    master_fd.get(), {.data = prompt_buffer.data(), .size = prompt_buffer.size()},
	    kPromptReadTimeoutMs);
	ok &= expect(prompt_bytes > 0, "restore failure test writes prompt to tty");
	constexpr std::array<char, 7> password{'s', 'e', 'c', 'r', 'e', 't', '\n'};
	ok &= expect(WriteAll(master_fd.get(), password.data(), password.size()),
	             "restore failure test writes password response");
	prompt_thread.join();

	ok &= expect(prompt_result == PAM_CONV_ERR,
	             "restore failure test returns conversation error after terminal restore error");
	ok &= expect(NativePromptConversationTestAccess::TerminalRestoreFailed(*conversation),
	             "restore failure test records terminal restore failure");
	ok &= expect(response == nullptr, "restore failure test returns no response");
	if (response != nullptr) {
		std::free(response);
	}
	return ok;
}
