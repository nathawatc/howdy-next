#include "prompt/prompt_coordinator_test_groups.hpp"
#include "prompt/prompt_coordinator_test_support.hpp"

namespace {
	using namespace howdy::test::prompt_coordinator;

	auto test_input_failure_callback_follows_password_completion(bool callback_throws) -> bool {
		FakeContext context{
		    .block_token_until_warning = true,
		    .fail_enter_send           = true,
		};
		const pid_t child_pid = spawn_child(EXIT_SUCCESS);
		if (!expect(child_pid > 0, "input failure callback child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;

		int               callback_calls               = 0;
		bool              callback_saw_completed_token = false;
		PromptCoordinator coordinator(nullptr, Workaround::Input, true, false,
		                              dependencies(&context), std::chrono::seconds(5));
		const auto        result = coordinator.run(make_compare_request(), [&] -> void {
			++callback_calls;
			{
				std::unique_lock<std::mutex> lock(context.token_mutex);
				callback_saw_completed_token =
				    context.auth_token_calls == 1 && !context.auth_token_active;
				context.warning_released_token = true;
			}
			context.token_condition.notify_one();
			if (callback_throws) {
				throw std::runtime_error("simulated warning failure");
			}
		});
		return expect(result.decision == PromptCoordinatorDecision::kHowdyResult,
		              "input failure preserves Howdy result") &&
		       expect(callback_calls == 1, "input failure callback runs exactly once") &&
		       expect(context.enter_device_constructions == 1,
		              "input failure creates one Enter device") &&
		       expect(context.enter_presses == 1, "input failure attempts one Enter press") &&
		       expect(callback_saw_completed_token,
		              "input failure callback runs after password request unwinds") &&
		       expect(child_reaped(child_pid), "input failure callback child is reaped");
	}

	auto test_input_restore_failure_suppresses_deferred_notice(
	    howdy::pam::ConversationRestoreResult restore_result, const std::string &message) -> bool {
		FakeContext context{
		    .block_token_until_warning = true,
		    .fail_enter_send           = true,
		    .secret_restore_result     = restore_result,
		};
		const pid_t child_pid = spawn_child(EXIT_SUCCESS);
		if (!expect(child_pid > 0, message + ": child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;

		int               callback_calls = 0;
		PromptCoordinator coordinator(nullptr, Workaround::Input, true, false,
		                              dependencies(&context), std::chrono::seconds(5));
		const auto result = coordinator.run(make_compare_request(), [&callback_calls] -> void {
			++callback_calls;
		});
		return expect(result.decision == PromptCoordinatorDecision::kPamResult,
		              message + ": hard failure overrides face success") &&
		       expect(result.pam_status == PAM_SYSTEM_ERR,
		              message + ": restoration failure returns system error") &&
		       expect(context.enter_presses == 1,
		              message + ": failed Enter attempt creates deferred notice") &&
		       expect(callback_calls == 0,
		              message + ": restoration failure suppresses deferred PAM notice") &&
		       expect(context.secret_restore_calls == 1,
		              message + ": conversation restoration attempted once") &&
		       expect(child_reaped(child_pid), message + ": child reaped");
	}

	auto test_input_success_sends_one_enter() -> bool {
		FakeContext context{
		    .block_token_until_warning = true,
		    .release_token_on_enter    = true,
		};
		const pid_t child_pid = spawn_child(EXIT_SUCCESS);
		if (!expect(child_pid > 0, "input success child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;
		context.run_thread     = std::this_thread::get_id();

		int               callback_calls = 0;
		PromptCoordinator coordinator(nullptr, Workaround::Input, true, false,
		                              dependencies(&context), std::chrono::seconds(5));
		const auto result = coordinator.run(make_compare_request(), [&callback_calls] -> void {
			++callback_calls;
		});
		return expect(result.decision == PromptCoordinatorDecision::kHowdyResult,
		              "input success preserves Howdy result") &&
		       expect(context.enter_device_constructions == 1,
		              "input success creates one Enter device") &&
		       expect(context.enter_presses == 1, "input success sends exactly one Enter press") &&
		       expect(context.auth_token_thread == context.run_thread,
		              "input success requests password on run caller thread") &&
		       expect(context.enter_thread == context.wait_thread &&
		                  context.enter_thread != context.run_thread,
		              "input success sends Enter from compare worker") &&
		       expect(callback_calls == 0, "input success emits no failure callback") &&
		       expect(child_reaped(child_pid), "input success child is reaped");
	}

	auto test_input_success_blocked_after_grace_waits_for_manual_completion() -> bool {
		FakeContext context{.block_token_until_warning = true};
		const pid_t child_pid = spawn_child(EXIT_SUCCESS);
		if (!expect(child_pid > 0, "blocked input success child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;

		int               callback_calls               = 0;
		bool              callback_saw_completed_token = false;
		PromptCoordinator coordinator(nullptr, Workaround::Input, true, false,
		                              dependencies(&context), std::chrono::seconds(5));
		const auto        result = coordinator.run(make_compare_request(), [&] -> void {
			++callback_calls;
			{
				std::unique_lock<std::mutex> lock(context.token_mutex);
				callback_saw_completed_token =
				    context.auth_token_calls == 1 && !context.auth_token_active;
				context.warning_released_token = true;
			}
			context.token_condition.notify_one();
		});
		return expect(result.decision == PromptCoordinatorDecision::kHowdyResult,
		              "blocked input success preserves Howdy result") &&
		       expect(context.enter_device_constructions == 1,
		              "blocked input success creates one Enter device") &&
		       expect(context.enter_presses == 1, "blocked input success sends one Enter press") &&
		       expect(callback_calls == 1, "blocked input success emits one failure callback") &&
		       expect(callback_saw_completed_token,
		              "blocked input callback runs after manual token completion") &&
		       expect(child_reaped(child_pid), "blocked input success child is reaped");
	}

	auto test_compare_failure_password_result(int pam_result, const std::string &label) -> bool {
		FakeContext context{
		    .token_delay  = std::chrono::milliseconds(100),
		    .token_result = pam_result,
		};
		const pid_t child_pid = spawn_child(static_cast<int>(CompareExit::kTimeoutReached));
		if (!expect(child_pid > 0, label + " child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;

		PromptCoordinator coordinator(nullptr, Workaround::Input, true, false,
		                              dependencies(&context), std::chrono::seconds(5));
		const auto        result          = coordinator.run(make_compare_request());
		const int         expected_status = static_cast<int>(CompareExit::kTimeoutReached) << 8;
		const bool        reaped          = child_reaped(child_pid);
		return expect(result.decision == PromptCoordinatorDecision::kPasswordFallback,
		              label + " returns password fallback") &&
		       expect(context.spawn_calls == 1 && context.spawned_pid == child_pid,
		              label + " spawns child once") &&
		       expect(context.wait_calls == 1 && context.waited_pid == child_pid,
		              label + " waits for spawned child") &&
		       expect(result.compare_status == expected_status,
		              label + " preserves exact compare status") &&
		       expect(result.pam_status == pam_result, label + " preserves PAM result") &&
		       expect(context.auth_token_calls == 1, label + " requests token once") &&
		       expect(context.terminate_calls == 0, label + " does not terminate child") &&
		       expect(reaped, label + " reaps child");
	}

	auto test_compare_signal_password_fallback() -> bool {
		FakeContext context{
		    .token_delay  = std::chrono::milliseconds(100),
		    .token_result = PAM_SUCCESS,
		};
		const pid_t child_pid = spawn_signaled_child(SIGTERM);
		if (!expect(child_pid > 0, "signaled compare child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;

		PromptCoordinator coordinator(nullptr, Workaround::Input, true, false,
		                              dependencies(&context), std::chrono::seconds(5));
		const auto        result = coordinator.run(make_compare_request());
		return expect(result.decision == PromptCoordinatorDecision::kPasswordFallback,
		              "signaled compare returns password fallback") &&
		       expect(WIFSIGNALED(result.compare_status),
		              "signaled compare preserves signaled wait status") &&
		       expect(WTERMSIG(result.compare_status) == SIGTERM,
		              "signaled compare preserves terminating signal") &&
		       expect(result.pam_status == PAM_SUCCESS, "signaled compare preserves PAM result") &&
		       expect(context.auth_token_calls == 1,
		              "signaled compare waits for password result") &&
		       expect(context.terminate_calls == 0,
		              "signaled compare does not terminate reaped child") &&
		       expect(child_reaped(child_pid), "signaled compare reaps child");
	}

	auto test_input_preflight_fallback() -> bool {
		FakeContext context{.preflight_result = false};
		const pid_t child_pid = spawn_child(EXIT_SUCCESS);
		if (!expect(child_pid > 0, "preflight-fallback child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;

		PromptCoordinator coordinator(nullptr, Workaround::Input, true, false,
		                              dependencies(&context), std::chrono::seconds(5));
		const auto        result = coordinator.run(make_compare_request());
		const bool        reaped = child_reaped(child_pid);
		return expect(result.decision == PromptCoordinatorDecision::kHowdyResult,
		              "preflight fallback returns compare result") &&
		       expect(context.spawn_calls == 1 && context.spawned_pid == child_pid,
		              "preflight fallback spawns child once") &&
		       expect(context.wait_calls == 1 && context.waited_pid == child_pid,
		              "preflight fallback waits for spawned child") &&
		       expect(context.preflight_calls == 1, "preflight fallback checks input once") &&
		       expect(context.auth_token_calls == 0,
		              "off fallback preserves standard non-parallel password behavior") &&
		       expect(context.terminate_calls == 0,
		              "preflight fallback does not terminate compare child") &&
		       expect(reaped, "preflight fallback reaps child");
	}

	auto test_enter_device_construction_fallback(bool return_null, const std::string &label)
	    -> bool {
		FakeContext context{
		    .fail_enter_construction  = !return_null,
		    .return_null_enter_device = return_null,
		};
		const pid_t child_pid = spawn_child(EXIT_SUCCESS);
		if (!expect(child_pid > 0, label + " child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;

		PromptCoordinator coordinator(nullptr, Workaround::Input, true, false,
		                              dependencies(&context), std::chrono::seconds(5));
		const auto        result = coordinator.run(make_compare_request());
		return expect(result.decision == PromptCoordinatorDecision::kHowdyResult,
		              label + " returns compare result") &&
		       expect(context.preflight_calls == 1, label + " runs input preflight once") &&
		       expect(context.enter_device_constructions == 1,
		              label + " calls Enter-device factory once") &&
		       expect(context.enter_presses == 0, label + " sends no Enter press") &&
		       expect(context.auth_token_calls == 0,
		              label + " falls back to standard PAM prompt") &&
		       expect(child_reaped(child_pid), label + " reaps child");
	}

	auto test_native_setup_without_input_fallback(bool available_result, int install_result,
	                                              const std::string &label) -> bool {
		FakeContext      context;
		NativePamFixture fixture(&context);
		if (!expect(fixture.start(false), label + " starts PAM handle")) {
			return false;
		}
		context.native_available      = available_result;
		context.native_install_result = install_result;
		const pid_t child_pid         = spawn_child(static_cast<int>(CompareExit::kTimeoutReached));
		if (!expect(child_pid > 0, label + " child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;

		PromptCoordinator coordinator(fixture.pamh(), Workaround::Native, true, false,
		                              dependencies(&context), std::chrono::seconds(5));
		const auto        result = coordinator.run(make_compare_request());
		return expect(result.decision == PromptCoordinatorDecision::kHowdyResult,
		              label + " returns compare result without input fallback") &&
		       expect(context.spawn_calls == 1 && context.spawned_pid == child_pid,
		              label + " spawns child once") &&
		       expect(context.wait_calls == 1 && context.waited_pid == child_pid,
		              label + " waits for spawned child") &&
		       expect(context.preflight_calls == 0, label + " skips input preflight") &&
		       expect(context.auth_token_calls == 0, label + " does not request token") &&
		       expect(context.terminate_calls == 0, label + " does not terminate child") &&
		       expect(context.original_conversation_calls == 0,
		              label + " does not invoke original conversation") &&
		       expect(child_reaped(child_pid), label + " reaps child");
	}

	auto test_native_input_success_uses_native_path() -> bool {
		FakeContext      context;
		NativePamFixture fixture(&context);
		if (!expect(fixture.start(false), "native-input success starts PAM handle")) {
			return false;
		}
		const pid_t child_pid = spawn_blocked_child();
		if (!expect(child_pid > 0, "native-input success child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;

		PromptCoordinator coordinator(fixture.pamh(), Workaround::NativeInput, true, false,
		                              dependencies(&context), std::chrono::seconds(5));
		const auto        result = coordinator.run(make_compare_request());
		return expect(result.decision == PromptCoordinatorDecision::kPamResult,
		              "native-input success uses native password task") &&
		       expect(result.pam_status == PAM_SUCCESS,
		              "native-input success preserves PAM success") &&
		       expect(context.spawn_calls == 1 && context.spawned_pid == child_pid,
		              "native-input success spawns child once") &&
		       expect(context.wait_calls == 1 && context.waited_pid == child_pid,
		              "native-input success waits for spawned child") &&
		       expect(context.preflight_calls == 0, "native-input success skips input preflight") &&
		       expect(context.auth_token_calls == 1, "native-input success requests token once") &&
		       expect(context.terminate_calls == 1 && context.terminated_pid == child_pid,
		              "native-input success terminates blocked compare child once") &&
		       expect(context.original_conversation_calls == 0,
		              "native-input success does not invoke original conversation") &&
		       expect(child_reaped(child_pid), "native-input success reaps compare child");
	}

	auto test_native_input_setup_fallback(bool available_result, int install_result,
	                                      const std::string &label) -> bool {
		FakeContext context{
		    .token_delay  = std::chrono::milliseconds(100),
		    .token_result = PAM_SUCCESS,
		};
		NativePamFixture fixture(&context);
		if (!expect(fixture.start(false), label + " starts PAM handle")) {
			return false;
		}
		context.native_available      = available_result;
		context.native_install_result = install_result;
		const pid_t child_pid         = spawn_child(static_cast<int>(CompareExit::kTimeoutReached));
		if (!expect(child_pid > 0, label + " child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;

		PromptCoordinator coordinator(fixture.pamh(), Workaround::NativeInput, true, false,
		                              dependencies(&context), std::chrono::seconds(5));
		const auto        result = coordinator.run(make_compare_request());
		return expect(result.decision == PromptCoordinatorDecision::kPasswordFallback,
		              label + " falls back to input password task") &&
		       expect(context.spawn_calls == 1 && context.spawned_pid == child_pid,
		              label + " spawns child once") &&
		       expect(context.wait_calls == 1 && context.waited_pid == child_pid,
		              label + " waits for spawned child") &&
		       expect(context.preflight_calls == 1, label + " runs input preflight once") &&
		       expect(context.auth_token_calls == 1, label + " requests token once") &&
		       expect(context.terminate_calls == 0, label + " does not terminate child") &&
		       expect(context.original_conversation_calls == 0,
		              label + " does not invoke original conversation") &&
		       expect(child_reaped(child_pid), label + " reaps child");
	}

	auto test_native_blocked_prompt_cleanup() -> bool {
		FakeContext context{.request_native_prompt = true};
		context.run_thread = std::this_thread::get_id();
		NativePamFixture fixture(&context);
		if (!expect(fixture.start(true), "blocked native prompt starts PAM PTY")) {
			return false;
		}
		const pid_t child_pid = spawn_child(EXIT_SUCCESS);
		if (!expect(child_pid > 0, "blocked native prompt child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;

		howdy::pam::PromptCoordinatorResult result;
		{
			PromptCoordinator coordinator(fixture.pamh(), Workaround::Native, true, false,
			                              dependencies(&context), std::chrono::seconds(5));
			result = coordinator.run(make_compare_request());
		}

		return expect(result.decision == PromptCoordinatorDecision::kHowdyResult,
		              "blocked native prompt returns compare result") &&
		       expect(context.spawn_calls == 1 && context.spawned_pid == child_pid,
		              "blocked native prompt spawns child once") &&
		       expect(context.wait_calls == 1 && context.waited_pid == child_pid,
		              "blocked native prompt waits for spawned child") &&
		       expect(result.compare_status == 0,
		              "blocked native prompt preserves successful compare status") &&
		       expect(context.native_prompt_seen,
		              "blocked native prompt reaches native conversation") &&
		       expect(context.auth_token_calls == 1, "blocked native prompt requests token once") &&
		       expect(context.terminate_calls == 0,
		              "blocked native prompt does not terminate compare child") &&
		       expect(context.original_conversation_calls == 0,
		              "blocked native prompt bypasses original conversation") &&
		       expect(context.native_abort_calls == 1,
		              "blocked native prompt requests one abort") &&
		       expect(context.native_create_thread == context.run_thread &&
		                  context.native_install_thread == context.run_thread &&
		                  context.auth_token_thread == context.run_thread &&
		                  context.native_restore_thread == context.run_thread,
		              "native PAM operations stay on run caller thread") &&
		       expect(context.native_abort_thread == context.wait_thread &&
		                  context.native_abort_thread != context.run_thread,
		              "native abort runs on compare worker") &&
		       expect(context.native_restore_calls == 1, "blocked native prompt restores once") &&
		       expect(child_reaped(child_pid), "blocked native prompt reaps child");
	}

	auto test_native_pam_wins() -> bool {
		FakeContext context{
		    .request_native_prompt  = true,
		    .complete_native_prompt = true,
		};
		NativePamFixture fixture(&context);
		if (!expect(fixture.start(true), "native PAM winner starts PAM PTY")) {
			return false;
		}
		const pid_t child_pid = spawn_blocked_child();
		if (!expect(child_pid > 0, "native PAM winner child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;

		howdy::pam::PromptCoordinatorResult result;
		{
			PromptCoordinator coordinator(fixture.pamh(), Workaround::Native, true, false,
			                              dependencies(&context), std::chrono::seconds(5));
			result = coordinator.run(make_compare_request());
		}
		return expect(result.decision == PromptCoordinatorDecision::kPamResult,
		              "native PAM winner returns PAM result") &&
		       expect(context.spawn_calls == 1 && context.spawned_pid == child_pid,
		              "native PAM winner spawns child once") &&
		       expect(context.wait_calls == 1 && context.waited_pid == child_pid,
		              "native PAM winner waits for spawned child") &&
		       expect(result.pam_status == PAM_SUCCESS,
		              "native PAM winner preserves PAM success") &&
		       expect(context.native_prompt_installed,
		              "native PAM winner observes installed native conversation") &&
		       expect(context.native_prompt_seen,
		              "native PAM winner observes native password prompt") &&
		       expect(context.native_prompt_input_sent,
		              "native PAM winner sends password input after prompt observation") &&
		       expect(context.native_prompt_completed,
		              "native PAM winner completes native conversation") &&
		       expect(context.pam_completion_observed_by_waiter,
		              "native PAM winner synchronizes completion before compare wait") &&
		       expect(context.auth_token_calls == 1,
		              "native PAM winner completes password task once") &&
		       expect(context.preflight_calls == 0, "native PAM winner skips input preflight") &&
		       expect(context.terminate_calls == 1 && context.terminated_pid == child_pid,
		              "native PAM winner terminates blocked compare child once") &&
		       expect(child_reaped(child_pid), "native PAM winner reaps compare child") &&
		       expect(context.native_abort_calls == 0,
		              "native PAM winner needs no abort after password_call_returned") &&
		       expect(context.native_restore_calls == 1,
		              "native PAM winner restores native prompt once") &&
		       expect(context.original_conversation_calls == 0,
		              "native PAM winner leaves no blocked native prompt task");
	}

	auto test_native_restore_failure_forces_system_error(
	    howdy::pam::ConversationRestoreResult restore_result, const std::string &message) -> bool {
		FakeContext context{
		    .request_native_prompt  = true,
		    .complete_native_prompt = true,
		    .native_restore_result  = restore_result,
		};
		NativePamFixture fixture(&context);
		if (!expect(fixture.start(true), message + ": starts PAM PTY")) {
			return false;
		}
		const pid_t child_pid = spawn_blocked_child();
		if (!expect(child_pid > 0, message + ": child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;

		PromptCoordinator coordinator(fixture.pamh(), Workaround::Native, true, false,
		                              dependencies(&context), std::chrono::seconds(5));
		const auto        result = coordinator.run(make_compare_request());
		return expect(result.decision == PromptCoordinatorDecision::kPamResult,
		              message + ": restoration failure overrides normal winner") &&
		       expect(result.pam_status == PAM_SYSTEM_ERR,
		              message + ": restoration failure returns hard PAM error") &&
		       expect(context.native_restore_calls == 1,
		              message + ": restoration attempted once") &&
		       expect(child_reaped(child_pid), message + ": child reaped");
	}
}  // namespace

auto run_prompt_mode_tests() -> bool {
	bool ok = true;
	ok &= test_input_failure_callback_follows_password_completion(false);
	ok &= test_input_failure_callback_follows_password_completion(true);
	ok &= test_input_restore_failure_suppresses_deferred_notice(
	    howdy::pam::ConversationRestoreResult::kFailClosedInstalled,
	    "input fail-closed restoration");
	ok &= test_input_restore_failure_suppresses_deferred_notice(
	    howdy::pam::ConversationRestoreResult::kUnsafe, "input unsafe restoration");
	ok &= test_input_success_sends_one_enter();
	ok &= test_input_success_blocked_after_grace_waits_for_manual_completion();
	ok &= test_compare_failure_password_result(PAM_SUCCESS, "successful password fallback");
	ok &= test_compare_failure_password_result(PAM_CONV_ERR, "failed password fallback");
	ok &= test_compare_signal_password_fallback();
	ok &= test_input_preflight_fallback();
	ok &= test_enter_device_construction_fallback(false, "Enter-device construction failure");
	ok &= test_enter_device_construction_fallback(true, "null Enter-device factory result");
	ok &= test_native_setup_without_input_fallback(false, -1, "native unavailable");
	ok &= test_native_setup_without_input_fallback(true, PAM_CONV_ERR, "native install failure");
	ok &= test_native_input_success_uses_native_path();
	ok &= test_native_input_setup_fallback(false, -1, "native-input unavailable");
	ok &= test_native_input_setup_fallback(true, PAM_CONV_ERR, "native-input install failure");
	ok &= test_native_blocked_prompt_cleanup();
	ok &= test_native_pam_wins();
	ok &= test_native_restore_failure_forces_system_error(
	    howdy::pam::ConversationRestoreResult::kFailClosedInstalled,
	    "fail-closed callback installation");
	ok &= test_native_restore_failure_forces_system_error(
	    howdy::pam::ConversationRestoreResult::kUnsafe, "unsafe native restoration");
	return ok;
}
