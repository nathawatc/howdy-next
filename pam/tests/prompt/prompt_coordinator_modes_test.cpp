#include "prompt/prompt_coordinator_fake.hpp"
#include "prompt/prompt_coordinator_native_support.hpp"
#include "prompt/prompt_coordinator_test_groups.hpp"
#include "support/process_test_support.hpp"

namespace {
	using namespace howdy::test::process;
	using namespace howdy::test::prompt_coordinator;

	auto test_existing_auth_token_skips_prompt_workaround() -> bool {
		FakeContext context;
		const pid_t child_pid = spawn_child(EXIT_SUCCESS);
		if (!expect(child_pid > 0, "existing-token child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;

		PromptCoordinator coordinator(nullptr, Workaround::kNativeInput, true, true,
		                              dependencies(&context), std::chrono::seconds(5));
		const auto        result = coordinator.run(make_compare_request());
		return expect(result.decision == PromptCoordinatorDecision::kHowdyResult,
		              "existing auth token preserves compare result") &&
		       expect(context.preflight_calls == 0, "existing auth token skips input preflight") &&
		       expect(context.prompt_submitter_constructions == 0,
		              "existing auth token skips prompt submitter construction") &&
		       expect(context.auth_token_calls == 0,
		              "existing auth token skips duplicate password request") &&
		       expect(child_reaped(child_pid), "existing-token child is reaped");
	}

	auto test_secret_observation_setup_fallback(int mode, const std::string &label) -> bool {
		FakeContext context;
		if (mode == 0) {
			context.return_null_secret_prompt = true;
		} else if (mode == 1) {
			context.secret_prompt_available = false;
		} else if (mode == 2) {
			context.secret_prompt_install_result = PAM_CONV_ERR;
		} else if (mode == 3) {
			context.throw_secret_prompt = true;
		} else {
			context.throw_secret_prompt_unknown = true;
		}
		const pid_t child_pid = spawn_child(EXIT_SUCCESS);
		if (!expect(child_pid > 0, label + " child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;

		PromptCoordinator coordinator(nullptr, Workaround::kInput, true, false,
		                              dependencies(&context), std::chrono::seconds(5));
		const auto        result = coordinator.run(make_compare_request());
		return expect(result.decision == PromptCoordinatorDecision::kHowdyResult,
		              label + " falls back to standard prompt") &&
		       expect(context.preflight_calls == 1, label + " runs input preflight once") &&
		       expect(context.prompt_submitter_constructions == 1,
		              label + " constructs prompt submitter once") &&
		       expect(context.auth_token_calls == 0, label + " does not request token") &&
		       expect(context.secret_restore_calls == 0,
		              label + " has no conversation to restore") &&
		       expect(child_reaped(child_pid), label + " reaps child");
	}

	auto test_native_prompt_setup_exception_fallback(bool unknown, const std::string &label)
	    -> bool {
		FakeContext      context;
		NativePamFixture fixture(&context);
		if (!expect(fixture.start(false), label + " starts PAM handle")) {
			return false;
		}
		context.throw_native_prompt         = !unknown;
		context.throw_native_prompt_unknown = unknown;
		const pid_t child_pid               = spawn_child(EXIT_SUCCESS);
		if (!expect(child_pid > 0, label + " child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;

		PromptCoordinator coordinator(fixture.pamh(), Workaround::kNative, true, false,
		                              dependencies(&context), std::chrono::seconds(5));
		const auto        result = coordinator.run(make_compare_request());
		return expect(result.decision == PromptCoordinatorDecision::kHowdyResult,
		              label + " falls back after native setup exception") &&
		       expect(context.preflight_calls == 0, label + " skips input fallback") &&
		       expect(context.auth_token_calls == 0, label + " does not request token") &&
		       expect(child_reaped(child_pid), label + " reaps child");
	}

	auto test_auth_token_exception_mapping(bool unknown, const std::string &label) -> bool {
		FakeContext context{
		    .throw_auth_token         = !unknown,
		    .throw_auth_token_unknown = unknown,
		};
		const pid_t child_pid = spawn_blocked_child();
		if (!expect(child_pid > 0, label + " child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;

		PromptCoordinator coordinator(nullptr, Workaround::kInput, true, false,
		                              dependencies(&context), std::chrono::seconds(5));
		const auto        result = coordinator.run(make_compare_request());
		return expect(result.decision == PromptCoordinatorDecision::kPamResult,
		              label + " returns PAM result") &&
		       expect(result.pam_status == PAM_SYSTEM_ERR,
		              label + " maps exception to system error") &&
		       expect(context.auth_token_calls == 1, label + " requests token once") &&
		       expect(child_reaped(child_pid), label + " reaps canceled compare child");
	}

	auto test_secret_restore_failure_mapping() -> bool {
		FakeContext context{
		    .secret_restore_result = howdy::pam::ConversationRestoreResult::kFailClosedInstalled,
		};
		const pid_t child_pid = spawn_child(EXIT_SUCCESS);
		if (!expect(child_pid > 0, "secret restore failure child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;

		PromptCoordinator coordinator(nullptr, Workaround::kInput, true, false,
		                              dependencies(&context), std::chrono::seconds(5));
		const auto        result = coordinator.run(make_compare_request());
		return expect(result.decision == PromptCoordinatorDecision::kPamResult,
		              "secret restore failure returns PAM result") &&
		       expect(result.pam_status == PAM_SYSTEM_ERR,
		              "secret restore failure maps to system error") &&
		       expect(context.secret_restore_calls == 1,
		              "secret restore failure attempts restoration once") &&
		       expect(child_reaped(child_pid), "secret restore failure child is reaped");
	}

	auto test_input_success_submits_prompt() -> bool {
		FakeContext context{
		    .block_token_until_release   = true,
		    .release_token_on_submission = true,
		};
		const pid_t child_pid = spawn_child(EXIT_SUCCESS);
		if (!expect(child_pid > 0, "input success child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;
		context.run_thread     = std::this_thread::get_id();

		PromptCoordinator coordinator(nullptr, Workaround::kInput, true, false,
		                              dependencies(&context), std::chrono::seconds(5));
		const auto        result = coordinator.run(make_compare_request());
		return expect(result.decision == PromptCoordinatorDecision::kHowdyResult,
		              "input success preserves Howdy result") &&
		       expect(context.prompt_submitter_constructions == 1,
		              "input success creates one prompt submitter") &&
		       expect(context.prompt_submissions == 1,
		              "input success submits prompt exactly once") &&
		       expect(context.auth_token_thread == context.run_thread,
		              "input success requests password on run caller thread") &&
		       expect(context.submission_thread == context.wait_thread &&
		                  context.submission_thread != context.run_thread,
		              "input success submits prompt from compare worker") &&
		       expect(child_reaped(child_pid), "input success child is reaped");
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

		PromptCoordinator coordinator(nullptr, Workaround::kInput, true, false,
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
		       expect(context.prompt_submissions == 0,
		              label + " submits no prompt after compare failure") &&
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

		PromptCoordinator coordinator(nullptr, Workaround::kInput, true, false,
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

		PromptCoordinator coordinator(nullptr, Workaround::kInput, true, false,
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

	auto test_prompt_submitter_construction_fallback(bool return_null, const std::string &label)
	    -> bool {
		FakeContext context{
		    .fail_prompt_submitter_construction = !return_null,
		    .return_null_prompt_submitter       = return_null,
		};
		const pid_t child_pid = spawn_child(EXIT_SUCCESS);
		if (!expect(child_pid > 0, label + " child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;

		PromptCoordinator coordinator(nullptr, Workaround::kInput, true, false,
		                              dependencies(&context), std::chrono::seconds(5));
		const auto        result = coordinator.run(make_compare_request());
		return expect(result.decision == PromptCoordinatorDecision::kHowdyResult,
		              label + " returns compare result") &&
		       expect(context.preflight_calls == 1, label + " runs input preflight once") &&
		       expect(context.prompt_submitter_constructions == 1,
		              label + " calls prompt submitter factory once") &&
		       expect(context.prompt_submissions == 0, label + " submits no prompt") &&
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

		PromptCoordinator coordinator(fixture.pamh(), Workaround::kNative, true, false,
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

		PromptCoordinator coordinator(fixture.pamh(), Workaround::kNativeInput, true, false,
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

		PromptCoordinator coordinator(fixture.pamh(), Workaround::kNativeInput, true, false,
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
		FakeContext context{
		    .token_result          = PAM_AUTHTOK_ERR,
		    .request_native_prompt = true,
		};
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
			PromptCoordinator coordinator(fixture.pamh(), Workaround::kNative, true, false,
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
		       expect(result.pam_status == PAM_AUTHTOK_ERR,
		              "blocked native prompt preserves mapped cancellation status") &&
		       expect(!context.native_terminal_restore_failed,
		              "blocked native prompt has no terminal restore failure") &&
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
			PromptCoordinator coordinator(fixture.pamh(), Workaround::kNative, true, false,
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
		       expect(!context.native_terminal_restore_failed,
		              "native PAM winner records no terminal restore failure") &&
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

	auto test_native_terminal_restore_failure_forces_system_error() -> bool {
		FakeContext context{
		    .token_result                             = PAM_AUTHTOK_ERR,
		    .request_native_prompt                    = true,
		    .native_terminal_restore_failure_on_abort = true,
		};
		NativePamFixture fixture(&context);
		if (!expect(fixture.start(true), "native terminal restore failure starts PAM PTY")) {
			return false;
		}
		const pid_t child_pid = spawn_child(EXIT_SUCCESS);
		if (!expect(child_pid > 0, "native terminal restore failure child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;

		PromptCoordinator coordinator(fixture.pamh(), Workaround::kNative, true, false,
		                              dependencies(&context), std::chrono::seconds(5));
		const auto        result = coordinator.run(make_compare_request());
		return expect(result.decision == PromptCoordinatorDecision::kPamResult,
		              "native terminal restore failure overrides successful face result") &&
		       expect(result.pam_status == PAM_SYSTEM_ERR,
		              "native terminal restore failure returns PAM_SYSTEM_ERR") &&
		       expect(context.native_terminal_restore_failed,
		              "native terminal restore failure is explicitly reported after abort") &&
		       expect(context.native_prompt_seen,
		              "native terminal restore failure reaches active native prompt") &&
		       expect(
		           context.native_abort_calls == 1,
		           "native terminal restore failure aborts native prompt after compare success") &&
		       expect(context.native_restore_calls == 1,
		              "native terminal restore failure restores PAM conversation once") &&
		       expect(child_reaped(child_pid), "native terminal restore failure reaps child");
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

		PromptCoordinator coordinator(fixture.pamh(), Workaround::kNative, true, false,
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
	ok &= test_existing_auth_token_skips_prompt_workaround();
	ok &= test_secret_observation_setup_fallback(0, "null secret observer");
	ok &= test_secret_observation_setup_fallback(1, "unavailable secret observer");
	ok &= test_secret_observation_setup_fallback(2, "secret observer install failure");
	ok &= test_secret_observation_setup_fallback(3, "secret observer setup exception");
	ok &= test_secret_observation_setup_fallback(4, "secret observer unknown exception");
	ok &= test_native_prompt_setup_exception_fallback(false, "native setup exception");
	ok &= test_native_prompt_setup_exception_fallback(true, "native setup unknown exception");
	ok &= test_auth_token_exception_mapping(false, "auth token exception");
	ok &= test_auth_token_exception_mapping(true, "auth token unknown exception");
	ok &= test_secret_restore_failure_mapping();
	ok &= test_input_success_submits_prompt();
	ok &= test_compare_failure_password_result(PAM_SUCCESS, "successful password fallback");
	ok &= test_compare_failure_password_result(PAM_CONV_ERR, "failed password fallback");
	ok &= test_compare_signal_password_fallback();
	ok &= test_input_preflight_fallback();
	ok &=
	    test_prompt_submitter_construction_fallback(false, "prompt submitter construction failure");
	ok &= test_prompt_submitter_construction_fallback(true, "null prompt submitter factory result");
	ok &= test_native_setup_without_input_fallback(false, -1, "native unavailable");
	ok &= test_native_setup_without_input_fallback(true, PAM_CONV_ERR, "native install failure");
	ok &= test_native_input_success_uses_native_path();
	ok &= test_native_input_setup_fallback(false, -1, "native-input unavailable");
	ok &= test_native_input_setup_fallback(true, PAM_CONV_ERR, "native-input install failure");
	ok &= test_native_blocked_prompt_cleanup();
	ok &= test_native_pam_wins();
	ok &= test_native_terminal_restore_failure_forces_system_error();
	ok &= test_native_restore_failure_forces_system_error(
	    howdy::pam::ConversationRestoreResult::kFailClosedInstalled,
	    "fail-closed callback installation");
	ok &= test_native_restore_failure_forces_system_error(
	    howdy::pam::ConversationRestoreResult::kUnsafe, "unsafe native restoration");
	return ok;
}
