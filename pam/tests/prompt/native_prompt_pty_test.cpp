#include "pam_test_support.hpp"
#include "prompt/native_prompt_conversation.hpp"
#include "prompt/native_prompt_test_groups.hpp"
#include "prompt/native_prompt_test_support.hpp"
#include "support/bit_flags.hpp"
#include "support/fd_io.hpp"
#include "test_support.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <termios.h>

#include <sys/types.h>

using howdy::test::Expect;

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

		if (!howdy::native::WriteAllToFd(master_fd.Get(), test_case.typeahead)) {
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
		(void)howdy::native::WriteAllToFd(master_fd.Get(), test_case.input);
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

		(void)howdy::native::WriteAllToFd(master_fd.Get(), "next\n");
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
	ok &= Expect(howdy::native::WriteAllToFd(master_fd.Get(), oversized_response),
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
	ok &= Expect(howdy::native::WriteAllToFd(master_fd.Get(),
	                                         std::string_view(password.data(), password.size())),
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
