#include "pam_test_support.hpp"
#include "prompt/native_prompt_test_support.hpp"
#include "test_support.hpp"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <thread>

#include <sys/wait.h>

auto ExpectCtrlCAbortsPromptAndRestoresTerminal() -> bool;
auto ExpectDispatchRejectsInvalidState() -> bool;
auto ExpectOriginalConversationRestored() -> bool;
auto ExpectRestoreHandlesNullPam() -> bool;
auto ExpectRestoreResult(std::array<int, 3>                    pam_set_results,
                         howdy::pam::ConversationRestoreResult expected, const std::string &message)
    -> bool;
auto ExpectDispatchThrowCleanup(int throw_mode, const std::string &message) -> bool;
auto ExpectDestroyedInstalledNativeWrapperFailsClosed() -> bool;
auto ExpectNativeTerminalEligibility() -> bool;
auto ExpectNativeTerminalAliases() -> bool;
auto ClosedStdinPreservedWithTerminalStdout() -> bool;
auto ClosedStdoutPreservedWithTerminalStdin() -> bool;
auto ClosedStderrPreservedWithTerminalStdin() -> bool;
auto ClosedStdinStdoutPreservedWithTerminalStderr() -> bool;
auto ClosedAllStdioRemainClosed() -> bool;
auto TtyNormalizationFailurePreservesClosedStdin() -> bool;
auto AbortPipeCreationFailureIsTransactional() -> bool;
auto AbortPipeFirstNormalizationFailureIsTransactional() -> bool;
auto AbortPipeSecondNormalizationFailureIsTransactional() -> bool;
auto ExpectInvalidPamTtyIsUnavailable() -> bool;
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

using howdy::test::Expect;

namespace {
	auto ExpectIsolated(bool (*scenario)(), const std::string &message) -> bool {
		const pid_t child_pid = fork();
		if (!Expect(child_pid >= 0, message + ": child spawned")) {
			return false;
		}
		if (child_pid == 0) {
			_exit(scenario() ? EXIT_SUCCESS : EXIT_FAILURE);
		}

		int status = 0;
		for (int attempt = 0; attempt < 500; ++attempt) {
			const pid_t waited = waitpid(child_pid, &status, WNOHANG);
			if (waited == child_pid) {
				return Expect(WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS,
				              message + ": child succeeds");
			}
			if (waited < 0 && errno != EINTR) {
				return Expect(false, message + ": waitpid failed, errno=" + std::to_string(errno));
			}
			(void)poll(nullptr, 0, 10);
		}

		(void)kill(child_pid, SIGTERM);
		for (int attempt = 0; attempt < 100; ++attempt) {
			const pid_t waited = waitpid(child_pid, &status, WNOHANG);
			if (waited == child_pid) {
				return Expect(false, message + ": child exceeded timeout and was terminated");
			}
			if (waited < 0 && errno != EINTR) {
				return Expect(
				    false, message + ": termination wait failed, errno=" + std::to_string(errno));
			}
			(void)poll(nullptr, 0, 10);
		}

		(void)kill(child_pid, SIGKILL);
		pid_t waited;
		do {
			waited = waitpid(child_pid, &status, 0);
		} while (waited < 0 && errno == EINTR);
		return Expect(waited == child_pid, message + ": timed-out child reaped") &&
		       Expect(false, message + ": child exceeded timeout and required SIGKILL");
	}

	auto NativePromptTestConv(int /*num_msg*/, const struct pam_message ** /*msgm*/,
	                          struct pam_response **response, void *appdata_ptr) -> int {
		if (response != nullptr) {
			*response = nullptr;
		}
		return appdata_ptr == nullptr ? PAM_CONV_ERR : PAM_SUCCESS;
	}

	struct LifecycleOperationContext {
		int                throw_mode = 0;
		std::array<int, 3> pam_set_results{{PAM_SUCCESS, PAM_SUCCESS, PAM_SUCCESS}};
		int                pam_set_calls = 0;
		struct pam_conv    last_pam_conversation{};
	};

	void NativePromptInjectedPostMessage(void *context) {
		const auto &operations = *static_cast<LifecycleOperationContext *>(context);
		if (operations.throw_mode == 1) {
			throw std::runtime_error("simulated dispatch failure");
		}
		if (operations.throw_mode == 2) {
			throw 1;
		}
	}

	auto InjectedSetPamItem(void *context, pam_handle_t * /*pamh*/, int item_type, const void *item)
	    -> int {
		auto &operations = *static_cast<LifecycleOperationContext *>(context);
		if (item_type == PAM_CONV && item != nullptr) {
			operations.last_pam_conversation = *static_cast<const struct pam_conv *>(item);
		}
		const auto index = static_cast<std::size_t>(operations.pam_set_calls++);
		return index < operations.pam_set_results.size() ? operations.pam_set_results[index]
		                                                 : PAM_SYSTEM_ERR;
	}

	auto CreateLifecycleConversation(NativePromptConversationTestAccess::Descriptors descriptors,
	                                 LifecycleOperationContext *operations = nullptr)
	    -> std::unique_ptr<NativePromptConversation> {
		return CreateConversation(std::move(descriptors),
		                          operations == nullptr
		                              ? NativePromptConversationTestAccess::Operations{}
		                              : NativePromptConversationTestAccess::Operations{
		                                    .context      = operations,
		                                    .post_message = NativePromptInjectedPostMessage,
		                                    .set_pam_item = InjectedSetPamItem,
		                                });
	}
}  // namespace

auto ExpectDispatchRejectsInvalidState() -> bool {
	bool ok = true;

	const struct pam_message message = {
	    .msg_style = PAM_TEXT_INFO,
	    .msg       = "notice",
	};
	const struct pam_message *message_ptr = &message;
	auto                     *responses   = reinterpret_cast<struct pam_response *>(0x1);

	ok &= Expect(NativePromptConversationTestAccess::Dispatch(1, &message_ptr, &responses,
	                                                          nullptr) == PAM_CONV_ERR,
	             "dispatch rejects null appdata");
	ok &= Expect(responses == nullptr, "dispatch clears response on null appdata");
	ok &= Expect(NativePromptConversationTestAccess::Dispatch(1, &message_ptr, nullptr, nullptr) ==
	                 PAM_CONV_ERR,
	             "dispatch rejects null response pointer");

	auto conversation = CreateConversation({});
	responses         = reinterpret_cast<struct pam_response *>(0x1);
	ok &= Expect(NativePromptConversationTestAccess::Dispatch(0, &message_ptr, &responses,
	                                                          conversation.get()) == PAM_CONV_ERR,
	             "dispatch rejects zero message count");
	ok &= Expect(responses == nullptr, "zero-message dispatch clears response");
	const struct pam_message *null_message = nullptr;
	responses                              = reinterpret_cast<struct pam_response *>(0x1);
	ok &= Expect(NativePromptConversationTestAccess::Dispatch(1, &null_message, &responses,
	                                                          conversation.get()) == PAM_CONV_ERR,
	             "dispatch rejects null message entry");
	ok &= Expect(responses == nullptr, "dispatch clears response for null message entry");

	return ok;
}

auto ExpectOriginalConversationRestored() -> bool {
	bool ok = true;

	ScopedFd                master_fd;
	ScopedFd                slave_fd;
	std::array<ScopedFd, 2> abort_pipe;
	ok &= Expect(OpenPtyPair(&master_fd, &slave_fd), "restore test opens pseudo terminal");
	ok &= Expect(OpenPipe(&abort_pipe), "restore test creates abort pipe");
	if (!ok) {
		return false;
	}

	int             appdata = 42;
	struct pam_conv original_conv{
	    .conv        = NativePromptTestConv,
	    .appdata_ptr = &appdata,
	};
	pam_handle_t *pamh = nullptr;
	if (howdy::test::PamStartForTest("howdy-native-test", "test-user", &original_conv, &pamh) !=
	        PAM_SUCCESS ||
	    pamh == nullptr) {
		return Expect(false, "restore test starts PAM handle");
	}

	{
		NativePromptConversation conversation(pamh);
		NativePromptConversationTestAccess::ReplaceDescriptors(
		    conversation, {.tty_fd         = std::move(slave_fd),
		                   .abort_read_fd  = std::move(abort_pipe[0]),
		                   .abort_write_fd = std::move(abort_pipe[1])});
		ok &= Expect(conversation.Available(), "restore test native prompt is available");
		ok &= Expect(conversation.Install() == PAM_SUCCESS,
		             "restore test installs native conversation");
		conversation.RestoreOriginal();
	}

	const void *restored_item = nullptr;
	ok &= Expect(pam_get_item(pamh, PAM_CONV, &restored_item) == PAM_SUCCESS,
	             "restore test reads PAM conversation");
	const auto *restored_conv = static_cast<const struct pam_conv *>(restored_item);
	ok &= Expect(restored_conv != nullptr, "restore test returns restored PAM conversation");
	ok &= Expect(restored_conv->conv == original_conv.conv,
	             "restore test restores original PAM conversation callback");
	ok &= Expect(restored_conv->appdata_ptr == original_conv.appdata_ptr,
	             "restore test restores original PAM conversation appdata");
	pam_end(pamh, PAM_SUCCESS);
	return ok;
}

auto ExpectRestoreHandlesNullPam() -> bool {
	bool                    ok = true;
	ScopedFd                master_fd;
	ScopedFd                slave_fd;
	std::array<ScopedFd, 2> abort_pipe;

	ok &= Expect(OpenPtyPair(&master_fd, &slave_fd), "null restore test opens pseudo terminal");
	ok &= Expect(OpenPipe(&abort_pipe), "null restore test creates abort pipe");
	if (!ok) {
		return false;
	}

	auto conversation = CreateConversation({.tty_fd         = std::move(slave_fd),
	                                        .abort_read_fd  = std::move(abort_pipe[0]),
	                                        .abort_write_fd = std::move(abort_pipe[1])});
	NativePromptConversationTestAccess::SetInstalled(*conversation, true);
	const auto result = conversation->RestoreOriginal();
	ok &= Expect(!NativePromptConversationTestAccess::Installed(*conversation),
	             "null PAM restore clears installed state");
	ok &= Expect(result == howdy::pam::ConversationRestoreResult::kUnsafe,
	             "null PAM restore reports unsafe detachment");
	return ok;
}

auto ExpectRestoreResult(std::array<int, 3>                    pam_set_results,
                         howdy::pam::ConversationRestoreResult expected, const std::string &message)
    -> bool {
	LifecycleOperationContext operations{.pam_set_results = pam_set_results};
	auto                      conversation = CreateLifecycleConversation({}, &operations);
	NativePromptConversationTestAccess::SetPamHandle(*conversation,
	                                                 reinterpret_cast<pam_handle_t *>(0x1));
	NativePromptConversationTestAccess::SetInstalled(*conversation, true);
	const struct pam_conv override =
	    NativePromptConversationTestAccess::OverrideConversation(*conversation);
	const auto result = conversation->RestoreOriginal();
	bool       ok     = Expect(result == expected, message + ": explicit restore result");
	const int  calls_before_destruction = operations.pam_set_calls;
	conversation.reset();
	ok &= Expect(operations.pam_set_calls == calls_before_destruction,
	             message + ": destructor performs no PAM operation");

	if (expected == howdy::pam::ConversationRestoreResult::kUnsafe) {
		const struct pam_message  message_item{.msg_style = PAM_TEXT_INFO, .msg = "late"};
		const struct pam_message *message_ptr = &message_item;
		auto                     *response    = reinterpret_cast<struct pam_response *>(0x1);
		ok &=
		    Expect(override.conv(1, &message_ptr, &response, override.appdata_ptr) == PAM_CONV_ERR,
		           message + ": retained callback fails closed after object destruction");
		ok &= Expect(response == nullptr,
		             message + ": retained callback clears response after destruction");
	} else if (expected == howdy::pam::ConversationRestoreResult::kFailClosedInstalled) {
		const struct pam_message  message_item{.msg_style = PAM_TEXT_INFO, .msg = "late"};
		const struct pam_message *message_ptr = &message_item;
		auto                     *response    = reinterpret_cast<struct pam_response *>(0x1);
		ok &= Expect(operations.last_pam_conversation.conv(
		                 1, &message_ptr, &response,
		                 operations.last_pam_conversation.appdata_ptr) == PAM_CONV_ERR,
		             message + ": installed static callback fails closed after destruction");
		ok &= Expect(response == nullptr, message + ": installed static callback clears response");
	}
	return ok;
}

auto ExpectDispatchThrowCleanup(int throw_mode, const std::string &message) -> bool {
	bool                    ok = true;
	ScopedFd                master_fd;
	ScopedFd                slave_fd;
	std::array<ScopedFd, 2> abort_pipe;

	ok &= Expect(OpenPtyPair(&master_fd, &slave_fd), message + ": opens pseudo terminal");
	ok &= Expect(OpenPipe(&abort_pipe), message + ": creates abort pipe");
	if (!ok) {
		return false;
	}

	LifecycleOperationContext operations{.throw_mode = throw_mode};
	auto conversation = CreateLifecycleConversation({.tty_fd         = std::move(slave_fd),
	                                                 .abort_read_fd  = std::move(abort_pipe[0]),
	                                                 .abort_write_fd = std::move(abort_pipe[1])},
	                                                &operations);

	const struct pam_message prompt = {
	    .msg_style = PAM_PROMPT_ECHO_OFF,
	    .msg       = "Password: ",
	};
	const struct pam_message *prompt_ptr      = &prompt;
	auto                     *responses       = reinterpret_cast<struct pam_response *>(0x1);
	int                       dispatch_result = PAM_SUCCESS;

	std::thread dispatch_thread([&] -> void {
		dispatch_result = NativePromptConversationTestAccess::Dispatch(1, &prompt_ptr, &responses,
		                                                               conversation.get());
	});

	std::array<char, 64> prompt_buffer{};
	const ssize_t        prompt_bytes = ReadWithTimeout(
	    master_fd.Get(), {.data = prompt_buffer.data(), .size = prompt_buffer.size()},
	    kPromptReadTimeoutMs);
	ok &= Expect(prompt_bytes > 0, message + ": prompt is written to tty");

	constexpr std::array<char, 7> password{'s', 'e', 'c', 'r', 'e', 't', '\n'};
	ok &= Expect(write(master_fd.Get(), password.data(), password.size()) ==
	                 static_cast<ssize_t>(password.size()),
	             message + ": writes password response");

	dispatch_thread.join();

	ok &= Expect(dispatch_result == PAM_CONV_ERR, message + ": dispatch fails closed");
	ok &= Expect(responses == nullptr, message + ": dispatch resets response");

	if (responses != nullptr) {
		std::free(responses);
	}
	return ok;
}

auto ExpectDestroyedInstalledNativeWrapperFailsClosed() -> bool {
	auto conversation = CreateConversation({});
	NativePromptConversationTestAccess::SetInstalled(*conversation, true);
	const auto installed = NativePromptConversationTestAccess::OverrideConversation(*conversation);
	conversation.reset();
	const struct pam_message  message{.msg_style = PAM_TEXT_INFO, .msg = "late"};
	const struct pam_message *message_ptr = &message;
	auto                     *response    = reinterpret_cast<struct pam_response *>(0x1);
	const int result = installed.conv(1, &message_ptr, &response, installed.appdata_ptr);
	return Expect(result == PAM_CONV_ERR, "destroyed installed native wrapper fails closed") &&
	       Expect(response == nullptr, "destroyed installed native wrapper clears response");
}

auto main() -> int {
	bool ok = true;

	ok &= ExpectCtrlCAbortsPromptAndRestoresTerminal();
	ok &= ExpectDispatchRejectsInvalidState();
	ok &= ExpectDestroyedInstalledNativeWrapperFailsClosed();
	ok &= ExpectNativeTerminalEligibility();
	ok &= ExpectNativeTerminalAliases();
	ok &= ExpectIsolated(ClosedStdinPreservedWithTerminalStdout,
	                     "closed stdin remains closed with terminal stdout");
	ok &= ExpectIsolated(ClosedStdoutPreservedWithTerminalStdin,
	                     "closed stdout remains closed with terminal stdin");
	ok &= ExpectIsolated(ClosedStderrPreservedWithTerminalStdin,
	                     "closed stderr remains closed with terminal stdin");
	ok &= ExpectIsolated(ClosedStdinStdoutPreservedWithTerminalStderr,
	                     "closed stdin and stdout remain closed with terminal stderr");
	ok &= ExpectIsolated(ClosedAllStdioRemainClosed,
	                     "all closed stdio remains closed and native stays unavailable");
	ok &= ExpectIsolated(TtyNormalizationFailurePreservesClosedStdin,
	                     "PAM_TTY normalization failure preserves closed stdin");
	ok &= ExpectIsolated(AbortPipeCreationFailureIsTransactional,
	                     "abort pipe creation failure is transactional");
	ok &= ExpectIsolated(AbortPipeFirstNormalizationFailureIsTransactional,
	                     "abort pipe read normalization failure is transactional");
	ok &= ExpectIsolated(AbortPipeSecondNormalizationFailureIsTransactional,
	                     "abort pipe write normalization failure is transactional");
	ok &= ExpectOriginalConversationRestored();
	ok &= ExpectInvalidPamTtyIsUnavailable();
	ok &= ExpectIsolated(ExpectAbortRequestUnblocksWithoutPipeWakeup, "abort wake test");
	ok &= ExpectIsolated(ExpectPtyHangupAbortsPrompt, "PTY hangup test");
	ok &= ExpectPollEintrWithoutAbortDoesNotAbortPrompt();
	ok &= ExpectPollEintrWithAbortFailsClosed();
	ok &= ExpectReadEintrRetriesAndAcceptsInput();
	ok &= ExpectReadEintrWithAbortFailsClosed();
	ok &= ExpectReadZeroRetriesAndAcceptsInput();
	ok &= ExpectRestoreEintrRetriesAndRestores();
	ok &= ExpectNativeMessageStyles();
	ok &= ExpectNativePromptInputEdges();
	ok &= ExpectOversizedPromptFailsClosed();
	ok &= ExpectRestoreFailureFailsClosed();
	ok &= ExpectRestoreHandlesNullPam();
	ok &= ExpectRestoreResult({PAM_SUCCESS, PAM_SUCCESS, PAM_SUCCESS},
	                          howdy::pam::ConversationRestoreResult::kOriginalRestored,
	                          "original conversation restoration");
	ok &= ExpectRestoreResult({PAM_SYSTEM_ERR, PAM_SUCCESS, PAM_SUCCESS},
	                          howdy::pam::ConversationRestoreResult::kFailClosedInstalled,
	                          "fail-closed restoration fallback");
	ok &= ExpectRestoreResult({PAM_SYSTEM_ERR, PAM_SYSTEM_ERR, PAM_SUCCESS},
	                          howdy::pam::ConversationRestoreResult::kUnsafe,
	                          "unsafe restoration fallback");
	ok &= ExpectDispatchThrowCleanup(1, "std exception after response allocation");
	ok &= ExpectDispatchThrowCleanup(2, "unknown exception after response allocation");

	return ok ? 0 : 1;
}
