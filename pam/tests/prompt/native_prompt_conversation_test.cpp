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

auto expect_ctrl_c_aborts_prompt_and_restores_terminal() -> bool;
auto expect_dispatch_rejects_invalid_state() -> bool;
auto expect_original_conversation_restored() -> bool;
auto expect_restore_handles_null_pam() -> bool;
auto expect_restore_result(std::array<int, 3>                    pam_set_results,
                           howdy::pam::ConversationRestoreResult expected,
                           const std::string                    &message) -> bool;
auto expect_dispatch_throw_cleanup(int throw_mode, const std::string &message) -> bool;
auto expect_destroyed_installed_native_wrapper_fails_closed() -> bool;
auto expect_native_terminal_eligibility() -> bool;
auto expect_native_terminal_aliases() -> bool;
auto closed_stdin_preserved_with_terminal_stdout() -> bool;
auto closed_stdout_preserved_with_terminal_stdin() -> bool;
auto closed_stderr_preserved_with_terminal_stdin() -> bool;
auto closed_stdin_stdout_preserved_with_terminal_stderr() -> bool;
auto closed_all_stdio_remain_closed() -> bool;
auto tty_normalization_failure_preserves_closed_stdin() -> bool;
auto abort_pipe_creation_failure_is_transactional() -> bool;
auto abort_pipe_first_normalization_failure_is_transactional() -> bool;
auto abort_pipe_second_normalization_failure_is_transactional() -> bool;
auto expect_invalid_pam_tty_is_unavailable() -> bool;
auto expect_abort_request_unblocks_without_pipe_wakeup() -> bool;
auto expect_pty_hangup_aborts_prompt() -> bool;
auto expect_poll_eintr_without_abort_does_not_abort_prompt() -> bool;
auto expect_poll_eintr_with_abort_fails_closed() -> bool;
auto expect_read_eintr_retries_and_accepts_input() -> bool;
auto expect_read_eintr_with_abort_fails_closed() -> bool;
auto expect_read_zero_retries_and_accepts_input() -> bool;
auto expect_restore_eintr_retries_and_restores() -> bool;
auto expect_native_message_styles() -> bool;
auto expect_native_prompt_input_edges() -> bool;
auto expect_oversized_prompt_fails_closed() -> bool;
auto expect_restore_failure_fails_closed() -> bool;

using howdy::test::expect;

namespace {
	auto expect_isolated(bool (*scenario)(), const std::string &message) -> bool {
		const pid_t child_pid = fork();
		if (!expect(child_pid >= 0, message + ": child spawned")) {
			return false;
		}
		if (child_pid == 0) {
			_exit(scenario() ? EXIT_SUCCESS : EXIT_FAILURE);
		}

		int status = 0;
		for (int attempt = 0; attempt < 500; ++attempt) {
			const pid_t waited = waitpid(child_pid, &status, WNOHANG);
			if (waited == child_pid) {
				return expect(WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS,
				              message + ": child succeeds");
			}
			if (waited < 0 && errno != EINTR) {
				return expect(false, message + ": waitpid failed, errno=" + std::to_string(errno));
			}
			(void)poll(nullptr, 0, 10);
		}

		(void)kill(child_pid, SIGTERM);
		for (int attempt = 0; attempt < 100; ++attempt) {
			const pid_t waited = waitpid(child_pid, &status, WNOHANG);
			if (waited == child_pid) {
				return expect(false, message + ": child exceeded timeout and was terminated");
			}
			if (waited < 0 && errno != EINTR) {
				return expect(
				    false, message + ": termination wait failed, errno=" + std::to_string(errno));
			}
			(void)poll(nullptr, 0, 10);
		}

		(void)kill(child_pid, SIGKILL);
		pid_t waited;
		do {
			waited = waitpid(child_pid, &status, 0);
		} while (waited < 0 && errno == EINTR);
		return expect(waited == child_pid, message + ": timed-out child reaped") &&
		       expect(false, message + ": child exceeded timeout and required SIGKILL");
	}

	auto native_prompt_test_conv(int /*num_msg*/, const struct pam_message ** /*msgm*/,
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

	void native_prompt_injected_post_message(void *context) {
		const auto &operations = *static_cast<LifecycleOperationContext *>(context);
		if (operations.throw_mode == 1) {
			throw std::runtime_error("simulated dispatch failure");
		}
		if (operations.throw_mode == 2) {
			throw 1;
		}
	}

	auto injected_set_pam_item(void *context, pam_handle_t * /*pamh*/, int item_type,
	                           const void *item) -> int {
		auto &operations = *static_cast<LifecycleOperationContext *>(context);
		if (item_type == PAM_CONV && item != nullptr) {
			operations.last_pam_conversation = *static_cast<const struct pam_conv *>(item);
		}
		const auto index = static_cast<std::size_t>(operations.pam_set_calls++);
		return index < operations.pam_set_results.size() ? operations.pam_set_results[index]
		                                                 : PAM_SYSTEM_ERR;
	}

	auto create_lifecycle_conversation(NativePromptConversationTestAccess::Descriptors descriptors,
	                                   LifecycleOperationContext *operations = nullptr)
	    -> std::unique_ptr<NativePromptConversation> {
		return create_conversation(descriptors,
		                           operations == nullptr
		                               ? NativePromptConversationTestAccess::Operations{}
		                               : NativePromptConversationTestAccess::Operations{
		                                     .context      = operations,
		                                     .post_message = native_prompt_injected_post_message,
		                                     .set_pam_item = injected_set_pam_item,
		                                 });
	}
}  // namespace

auto expect_dispatch_rejects_invalid_state() -> bool {
	bool ok = true;

	const struct pam_message message = {
	    .msg_style = PAM_TEXT_INFO,
	    .msg       = "notice",
	};
	const struct pam_message *message_ptr = &message;
	auto                     *responses   = reinterpret_cast<struct pam_response *>(0x1);

	ok &= expect(NativePromptConversationTestAccess::dispatch(1, &message_ptr, &responses,
	                                                          nullptr) == PAM_CONV_ERR,
	             "dispatch rejects null appdata");
	ok &= expect(responses == nullptr, "dispatch clears response on null appdata");
	ok &= expect(NativePromptConversationTestAccess::dispatch(1, &message_ptr, nullptr, nullptr) ==
	                 PAM_CONV_ERR,
	             "dispatch rejects null response pointer");

	auto conversation = create_conversation({});
	responses         = reinterpret_cast<struct pam_response *>(0x1);
	ok &= expect(NativePromptConversationTestAccess::dispatch(0, &message_ptr, &responses,
	                                                          conversation.get()) == PAM_CONV_ERR,
	             "dispatch rejects zero message count");
	ok &= expect(responses == nullptr, "zero-message dispatch clears response");
	const struct pam_message *null_message = nullptr;
	responses                              = reinterpret_cast<struct pam_response *>(0x1);
	ok &= expect(NativePromptConversationTestAccess::dispatch(1, &null_message, &responses,
	                                                          conversation.get()) == PAM_CONV_ERR,
	             "dispatch rejects null message entry");
	ok &= expect(responses == nullptr, "dispatch clears response for null message entry");

	return ok;
}

auto expect_original_conversation_restored() -> bool {
	bool ok = true;

	ScopedFd                master_fd;
	ScopedFd                slave_fd;
	std::array<ScopedFd, 2> abort_pipe;
	ok &= expect(open_pty_pair(&master_fd, &slave_fd), "restore test opens pseudo terminal");
	ok &= expect(open_pipe(&abort_pipe), "restore test creates abort pipe");
	if (!ok) {
		return false;
	}

	int             appdata = 42;
	struct pam_conv original_conv{
	    .conv        = native_prompt_test_conv,
	    .appdata_ptr = &appdata,
	};
	pam_handle_t *pamh = nullptr;
	if (pam_start("howdy-native-test", "test-user", &original_conv, &pamh) != PAM_SUCCESS ||
	    pamh == nullptr) {
		return expect(false, "restore test starts PAM handle");
	}

	{
		NativePromptConversation conversation(pamh);
		NativePromptConversationTestAccess::replace_descriptors(
		    conversation, {.tty_fd         = slave_fd.release(),
		                   .abort_read_fd  = abort_pipe[0].release(),
		                   .abort_write_fd = abort_pipe[1].release()});
		ok &= expect(conversation.available(), "restore test native prompt is available");
		ok &= expect(conversation.install() == PAM_SUCCESS,
		             "restore test installs native conversation");
		conversation.restore_original();
	}

	const void *restored_item = nullptr;
	ok &= expect(pam_get_item(pamh, PAM_CONV, &restored_item) == PAM_SUCCESS,
	             "restore test reads PAM conversation");
	const auto *restored_conv = static_cast<const struct pam_conv *>(restored_item);
	ok &= expect(restored_conv != nullptr, "restore test returns restored PAM conversation");
	ok &= expect(restored_conv->conv == original_conv.conv,
	             "restore test restores original PAM conversation callback");
	ok &= expect(restored_conv->appdata_ptr == original_conv.appdata_ptr,
	             "restore test restores original PAM conversation appdata");
	pam_end(pamh, PAM_SUCCESS);
	return ok;
}

auto expect_restore_handles_null_pam() -> bool {
	bool                    ok = true;
	ScopedFd                master_fd;
	ScopedFd                slave_fd;
	std::array<ScopedFd, 2> abort_pipe;

	ok &= expect(open_pty_pair(&master_fd, &slave_fd), "null restore test opens pseudo terminal");
	ok &= expect(open_pipe(&abort_pipe), "null restore test creates abort pipe");
	if (!ok) {
		return false;
	}

	auto conversation = create_conversation({.tty_fd         = slave_fd.release(),
	                                         .abort_read_fd  = abort_pipe[0].release(),
	                                         .abort_write_fd = abort_pipe[1].release()});
	NativePromptConversationTestAccess::set_installed(*conversation, true);
	const auto result = conversation->restore_original();
	ok &= expect(!NativePromptConversationTestAccess::installed(*conversation),
	             "null PAM restore clears installed state");
	ok &= expect(result == howdy::pam::ConversationRestoreResult::kUnsafe,
	             "null PAM restore reports unsafe detachment");
	return ok;
}

auto expect_restore_result(std::array<int, 3>                    pam_set_results,
                           howdy::pam::ConversationRestoreResult expected,
                           const std::string                    &message) -> bool {
	LifecycleOperationContext operations{.pam_set_results = pam_set_results};
	auto                      conversation = create_lifecycle_conversation({}, &operations);
	NativePromptConversationTestAccess::set_pam_handle(*conversation,
	                                                   reinterpret_cast<pam_handle_t *>(0x1));
	NativePromptConversationTestAccess::set_installed(*conversation, true);
	const struct pam_conv override =
	    NativePromptConversationTestAccess::override_conversation(*conversation);
	const auto result = conversation->restore_original();
	bool       ok     = expect(result == expected, message + ": explicit restore result");
	const int  calls_before_destruction = operations.pam_set_calls;
	conversation.reset();
	ok &= expect(operations.pam_set_calls == calls_before_destruction,
	             message + ": destructor performs no PAM operation");

	if (expected == howdy::pam::ConversationRestoreResult::kUnsafe) {
		const struct pam_message  message_item{.msg_style = PAM_TEXT_INFO, .msg = "late"};
		const struct pam_message *message_ptr = &message_item;
		auto                     *response    = reinterpret_cast<struct pam_response *>(0x1);
		ok &=
		    expect(override.conv(1, &message_ptr, &response, override.appdata_ptr) == PAM_CONV_ERR,
		           message + ": retained callback fails closed after object destruction");
		ok &= expect(response == nullptr,
		             message + ": retained callback clears response after destruction");
	} else if (expected == howdy::pam::ConversationRestoreResult::kFailClosedInstalled) {
		const struct pam_message  message_item{.msg_style = PAM_TEXT_INFO, .msg = "late"};
		const struct pam_message *message_ptr = &message_item;
		auto                     *response    = reinterpret_cast<struct pam_response *>(0x1);
		ok &= expect(operations.last_pam_conversation.conv(
		                 1, &message_ptr, &response,
		                 operations.last_pam_conversation.appdata_ptr) == PAM_CONV_ERR,
		             message + ": installed static callback fails closed after destruction");
		ok &= expect(response == nullptr, message + ": installed static callback clears response");
	}
	return ok;
}

auto expect_dispatch_throw_cleanup(int throw_mode, const std::string &message) -> bool {
	bool                    ok = true;
	ScopedFd                master_fd;
	ScopedFd                slave_fd;
	std::array<ScopedFd, 2> abort_pipe;

	ok &= expect(open_pty_pair(&master_fd, &slave_fd), message + ": opens pseudo terminal");
	ok &= expect(open_pipe(&abort_pipe), message + ": creates abort pipe");
	if (!ok) {
		return false;
	}

	LifecycleOperationContext operations{.throw_mode = throw_mode};
	auto conversation = create_lifecycle_conversation({.tty_fd         = slave_fd.release(),
	                                                   .abort_read_fd  = abort_pipe[0].release(),
	                                                   .abort_write_fd = abort_pipe[1].release()},
	                                                  &operations);

	const struct pam_message prompt = {
	    .msg_style = PAM_PROMPT_ECHO_OFF,
	    .msg       = "Password: ",
	};
	const struct pam_message *prompt_ptr      = &prompt;
	auto                     *responses       = reinterpret_cast<struct pam_response *>(0x1);
	int                       dispatch_result = PAM_SUCCESS;

	std::thread dispatch_thread([&] -> void {
		dispatch_result = NativePromptConversationTestAccess::dispatch(1, &prompt_ptr, &responses,
		                                                               conversation.get());
	});

	std::array<char, 64> prompt_buffer{};
	const ssize_t        prompt_bytes = read_with_timeout(
	    master_fd.get(), {.data = prompt_buffer.data(), .size = prompt_buffer.size()},
	    kPromptReadTimeoutMs);
	ok &= expect(prompt_bytes > 0, message + ": prompt is written to tty");

	constexpr std::array<char, 7> kPassword{'s', 'e', 'c', 'r', 'e', 't', '\n'};
	ok &= expect(write(master_fd.get(), kPassword.data(), kPassword.size()) ==
	                 static_cast<ssize_t>(kPassword.size()),
	             message + ": writes password response");

	dispatch_thread.join();

	ok &= expect(dispatch_result == PAM_CONV_ERR, message + ": dispatch fails closed");
	ok &= expect(responses == nullptr, message + ": dispatch resets response");

	if (responses != nullptr) {
		std::free(responses);
	}
	return ok;
}

auto expect_destroyed_installed_native_wrapper_fails_closed() -> bool {
	auto conversation = create_conversation({});
	NativePromptConversationTestAccess::set_installed(*conversation, true);
	const auto installed = NativePromptConversationTestAccess::override_conversation(*conversation);
	conversation.reset();
	const struct pam_message  message{.msg_style = PAM_TEXT_INFO, .msg = "late"};
	const struct pam_message *message_ptr = &message;
	auto                     *response    = reinterpret_cast<struct pam_response *>(0x1);
	const int result = installed.conv(1, &message_ptr, &response, installed.appdata_ptr);
	return expect(result == PAM_CONV_ERR, "destroyed installed native wrapper fails closed") &&
	       expect(response == nullptr, "destroyed installed native wrapper clears response");
}

auto main() -> int {
	bool ok = true;

	ok &= expect_ctrl_c_aborts_prompt_and_restores_terminal();
	ok &= expect_dispatch_rejects_invalid_state();
	ok &= expect_destroyed_installed_native_wrapper_fails_closed();
	ok &= expect_native_terminal_eligibility();
	ok &= expect_native_terminal_aliases();
	ok &= expect_isolated(closed_stdin_preserved_with_terminal_stdout,
	                      "closed stdin remains closed with terminal stdout");
	ok &= expect_isolated(closed_stdout_preserved_with_terminal_stdin,
	                      "closed stdout remains closed with terminal stdin");
	ok &= expect_isolated(closed_stderr_preserved_with_terminal_stdin,
	                      "closed stderr remains closed with terminal stdin");
	ok &= expect_isolated(closed_stdin_stdout_preserved_with_terminal_stderr,
	                      "closed stdin and stdout remain closed with terminal stderr");
	ok &= expect_isolated(closed_all_stdio_remain_closed,
	                      "all closed stdio remains closed and native stays unavailable");
	ok &= expect_isolated(tty_normalization_failure_preserves_closed_stdin,
	                      "PAM_TTY normalization failure preserves closed stdin");
	ok &= expect_isolated(abort_pipe_creation_failure_is_transactional,
	                      "abort pipe creation failure is transactional");
	ok &= expect_isolated(abort_pipe_first_normalization_failure_is_transactional,
	                      "abort pipe read normalization failure is transactional");
	ok &= expect_isolated(abort_pipe_second_normalization_failure_is_transactional,
	                      "abort pipe write normalization failure is transactional");
	ok &= expect_original_conversation_restored();
	ok &= expect_invalid_pam_tty_is_unavailable();
	ok &= expect_isolated(expect_abort_request_unblocks_without_pipe_wakeup, "abort wake test");
	ok &= expect_isolated(expect_pty_hangup_aborts_prompt, "PTY hangup test");
	ok &= expect_poll_eintr_without_abort_does_not_abort_prompt();
	ok &= expect_poll_eintr_with_abort_fails_closed();
	ok &= expect_read_eintr_retries_and_accepts_input();
	ok &= expect_read_eintr_with_abort_fails_closed();
	ok &= expect_read_zero_retries_and_accepts_input();
	ok &= expect_restore_eintr_retries_and_restores();
	ok &= expect_native_message_styles();
	ok &= expect_native_prompt_input_edges();
	ok &= expect_oversized_prompt_fails_closed();
	ok &= expect_restore_failure_fails_closed();
	ok &= expect_restore_handles_null_pam();
	ok &= expect_restore_result({PAM_SUCCESS, PAM_SUCCESS, PAM_SUCCESS},
	                            howdy::pam::ConversationRestoreResult::kOriginalRestored,
	                            "original conversation restoration");
	ok &= expect_restore_result({PAM_SYSTEM_ERR, PAM_SUCCESS, PAM_SUCCESS},
	                            howdy::pam::ConversationRestoreResult::kFailClosedInstalled,
	                            "fail-closed restoration fallback");
	ok &= expect_restore_result({PAM_SYSTEM_ERR, PAM_SYSTEM_ERR, PAM_SUCCESS},
	                            howdy::pam::ConversationRestoreResult::kUnsafe,
	                            "unsafe restoration fallback");
	ok &= expect_dispatch_throw_cleanup(1, "std exception after response allocation");
	ok &= expect_dispatch_throw_cleanup(2, "unknown exception after response allocation");

	return ok ? 0 : 1;
}
