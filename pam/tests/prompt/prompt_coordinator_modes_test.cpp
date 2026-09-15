#include "prompt/prompt_coordinator_fake.hpp"
#include "prompt/prompt_coordinator_native_support.hpp"
#include "prompt/prompt_coordinator_test_groups.hpp"
#include "support/process_test_support.hpp"

namespace {
	using namespace howdy::test::process;
	using namespace howdy::test::prompt_coordinator;

	auto TestExistingAuthTokenSkipsPromptWorkaround() -> bool {
		FakeContext context;
		const pid_t child_pid = SpawnChild(EXIT_SUCCESS);
		if (!Expect(child_pid > 0, "existing-token child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;

		PromptCoordinator coordinator(nullptr, Workaround::kNativeInput, true, true,
		                              Dependencies(&context), std::chrono::seconds(5));
		const auto        result = coordinator.Run(MakeCompareRequest());
		return Expect(result.decision == PromptCoordinatorDecision::kHowdyResult,
		              "existing auth token preserves compare result") &&
		       Expect(context.preflight_calls == 0, "existing auth token skips input preflight") &&
		       Expect(context.prompt_submitter_constructions == 0,
		              "existing auth token skips prompt submitter construction") &&
		       Expect(context.auth_token_calls == 0,
		              "existing auth token skips duplicate password request") &&
		       Expect(ChildReaped(child_pid), "existing-token child is reaped");
	}

	auto TestSecretObservationSetupFallback(int mode, const std::string &label) -> bool {
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
		const pid_t child_pid = SpawnChild(EXIT_SUCCESS);
		if (!Expect(child_pid > 0, label + " child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;

		PromptCoordinator coordinator(nullptr, Workaround::kInput, true, false,
		                              Dependencies(&context), std::chrono::seconds(5));
		const auto        result = coordinator.Run(MakeCompareRequest());
		return Expect(result.decision == PromptCoordinatorDecision::kHowdyResult,
		              label + " falls back to standard prompt") &&
		       Expect(context.preflight_calls == 1, label + " runs input preflight once") &&
		       Expect(context.prompt_submitter_constructions == 1,
		              label + " constructs prompt submitter once") &&
		       Expect(context.auth_token_calls == 0, label + " does not request token") &&
		       Expect(context.secret_restore_calls == 0,
		              label + " has no conversation to restore") &&
		       Expect(ChildReaped(child_pid), label + " reaps child");
	}

	auto TestNativePromptSetupExceptionFallback(bool unknown, const std::string &label) -> bool {
		FakeContext      context;
		NativePamFixture fixture(&context);
		if (!Expect(fixture.Start(false), label + " starts PAM handle")) {
			return false;
		}
		context.throw_native_prompt         = !unknown;
		context.throw_native_prompt_unknown = unknown;
		const pid_t child_pid               = SpawnChild(EXIT_SUCCESS);
		if (!Expect(child_pid > 0, label + " child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;

		PromptCoordinator coordinator(fixture.Pamh(), Workaround::kNative, true, false,
		                              Dependencies(&context), std::chrono::seconds(5));
		const auto        result = coordinator.Run(MakeCompareRequest());
		return Expect(result.decision == PromptCoordinatorDecision::kHowdyResult,
		              label + " falls back after native setup exception") &&
		       Expect(context.preflight_calls == 0, label + " skips input fallback") &&
		       Expect(context.auth_token_calls == 0, label + " does not request token") &&
		       Expect(ChildReaped(child_pid), label + " reaps child");
	}

	auto TestAuthTokenExceptionMapping(bool unknown, const std::string &label) -> bool {
		FakeContext context{
		    .throw_auth_token         = !unknown,
		    .throw_auth_token_unknown = unknown,
		};
		const pid_t child_pid = SpawnBlockedChild();
		if (!Expect(child_pid > 0, label + " child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;

		PromptCoordinator coordinator(nullptr, Workaround::kInput, true, false,
		                              Dependencies(&context), std::chrono::seconds(5));
		const auto        result = coordinator.Run(MakeCompareRequest());
		return Expect(result.decision == PromptCoordinatorDecision::kPamResult,
		              label + " returns PAM result") &&
		       Expect(result.pam_status == PAM_SYSTEM_ERR,
		              label + " maps exception to system error") &&
		       Expect(context.auth_token_calls == 1, label + " requests token once") &&
		       Expect(ChildReaped(child_pid), label + " reaps canceled compare child");
	}

	auto TestSecretRestoreFailureMapping() -> bool {
		FakeContext context{
		    .secret_restore_result = howdy::pam::ConversationRestoreResult::kFailClosedInstalled,
		};
		const pid_t child_pid = SpawnChild(EXIT_SUCCESS);
		if (!Expect(child_pid > 0, "secret restore failure child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;

		PromptCoordinator coordinator(nullptr, Workaround::kInput, true, false,
		                              Dependencies(&context), std::chrono::seconds(5));
		const auto        result = coordinator.Run(MakeCompareRequest());
		return Expect(result.decision == PromptCoordinatorDecision::kPamResult,
		              "secret restore failure returns PAM result") &&
		       Expect(result.pam_status == PAM_SYSTEM_ERR,
		              "secret restore failure maps to system error") &&
		       Expect(context.secret_restore_calls == 1,
		              "secret restore failure attempts restoration once") &&
		       Expect(ChildReaped(child_pid), "secret restore failure child is reaped");
	}

	auto TestInputSuccessSubmitsPrompt() -> bool {
		FakeContext context{
		    .block_token_until_release   = true,
		    .release_token_on_submission = true,
		};
		const pid_t child_pid = SpawnChild(EXIT_SUCCESS);
		if (!Expect(child_pid > 0, "input success child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;
		context.run_thread     = std::this_thread::get_id();

		PromptCoordinator coordinator(nullptr, Workaround::kInput, true, false,
		                              Dependencies(&context), std::chrono::seconds(5));
		const auto        result = coordinator.Run(MakeCompareRequest());
		return Expect(result.decision == PromptCoordinatorDecision::kHowdyResult,
		              "input success preserves Howdy result") &&
		       Expect(context.prompt_submitter_constructions == 1,
		              "input success creates one prompt submitter") &&
		       Expect(context.prompt_submissions == 1,
		              "input success submits prompt exactly once") &&
		       Expect(context.auth_token_thread == context.run_thread,
		              "input success requests password on run caller thread") &&
		       Expect(context.submission_thread == context.wait_thread &&
		                  context.submission_thread != context.run_thread,
		              "input success submits prompt from compare worker") &&
		       Expect(ChildReaped(child_pid), "input success child is reaped");
	}

	auto TestCompareFailurePasswordResult(int pam_result, const std::string &label) -> bool {
		FakeContext context{
		    .token_delay  = std::chrono::milliseconds(100),
		    .token_result = pam_result,
		};
		const pid_t child_pid = SpawnChild(static_cast<int>(CompareExit::kTimeoutReached));
		if (!Expect(child_pid > 0, label + " child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;

		PromptCoordinator coordinator(nullptr, Workaround::kInput, true, false,
		                              Dependencies(&context), std::chrono::seconds(5));
		const auto        result          = coordinator.Run(MakeCompareRequest());
		const int         expected_status = static_cast<int>(CompareExit::kTimeoutReached) << 8;
		const bool        reaped          = ChildReaped(child_pid);
		return Expect(result.decision == PromptCoordinatorDecision::kPasswordFallback,
		              label + " returns password fallback") &&
		       Expect(context.spawn_calls == 1 && context.spawned_pid == child_pid,
		              label + " spawns child once") &&
		       Expect(context.wait_calls == 1 && context.waited_pid == child_pid,
		              label + " waits for spawned child") &&
		       Expect(result.compare_status == expected_status,
		              label + " preserves exact compare status") &&
		       Expect(result.pam_status == pam_result, label + " preserves PAM result") &&
		       Expect(context.auth_token_calls == 1, label + " requests token once") &&
		       Expect(context.prompt_submissions == 0,
		              label + " submits no prompt after compare failure") &&
		       Expect(context.terminate_calls == 0, label + " does not terminate child") &&
		       Expect(reaped, label + " reaps child");
	}

	auto TestCompareSignalPasswordFallback() -> bool {
		FakeContext context{
		    .token_delay  = std::chrono::milliseconds(100),
		    .token_result = PAM_SUCCESS,
		};
		const pid_t child_pid = SpawnSignaledChild(SIGTERM);
		if (!Expect(child_pid > 0, "signaled compare child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;

		PromptCoordinator coordinator(nullptr, Workaround::kInput, true, false,
		                              Dependencies(&context), std::chrono::seconds(5));
		const auto        result = coordinator.Run(MakeCompareRequest());
		return Expect(result.decision == PromptCoordinatorDecision::kPasswordFallback,
		              "signaled compare returns password fallback") &&
		       Expect(WIFSIGNALED(result.compare_status),
		              "signaled compare preserves signaled wait status") &&
		       Expect(WTERMSIG(result.compare_status) == SIGTERM,
		              "signaled compare preserves terminating signal") &&
		       Expect(result.pam_status == PAM_SUCCESS, "signaled compare preserves PAM result") &&
		       Expect(context.auth_token_calls == 1,
		              "signaled compare waits for password result") &&
		       Expect(context.terminate_calls == 0,
		              "signaled compare does not terminate reaped child") &&
		       Expect(ChildReaped(child_pid), "signaled compare reaps child");
	}

	auto TestInputPreflightFallback() -> bool {
		FakeContext context{.preflight_result = false};
		const pid_t child_pid = SpawnChild(EXIT_SUCCESS);
		if (!Expect(child_pid > 0, "preflight-fallback child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;

		PromptCoordinator coordinator(nullptr, Workaround::kInput, true, false,
		                              Dependencies(&context), std::chrono::seconds(5));
		const auto        result = coordinator.Run(MakeCompareRequest());
		const bool        reaped = ChildReaped(child_pid);
		return Expect(result.decision == PromptCoordinatorDecision::kHowdyResult,
		              "preflight fallback returns compare result") &&
		       Expect(context.spawn_calls == 1 && context.spawned_pid == child_pid,
		              "preflight fallback spawns child once") &&
		       Expect(context.wait_calls == 1 && context.waited_pid == child_pid,
		              "preflight fallback waits for spawned child") &&
		       Expect(context.preflight_calls == 1, "preflight fallback checks input once") &&
		       Expect(context.auth_token_calls == 0,
		              "off fallback preserves standard non-parallel password behavior") &&
		       Expect(context.terminate_calls == 0,
		              "preflight fallback does not terminate compare child") &&
		       Expect(reaped, "preflight fallback reaps child");
	}

	auto TestPromptSubmitterConstructionFallback(bool return_null, const std::string &label)
	    -> bool {
		FakeContext context{
		    .fail_prompt_submitter_construction = !return_null,
		    .return_null_prompt_submitter       = return_null,
		};
		const pid_t child_pid = SpawnChild(EXIT_SUCCESS);
		if (!Expect(child_pid > 0, label + " child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;

		PromptCoordinator coordinator(nullptr, Workaround::kInput, true, false,
		                              Dependencies(&context), std::chrono::seconds(5));
		const auto        result = coordinator.Run(MakeCompareRequest());
		return Expect(result.decision == PromptCoordinatorDecision::kHowdyResult,
		              label + " returns compare result") &&
		       Expect(context.preflight_calls == 1, label + " runs input preflight once") &&
		       Expect(context.prompt_submitter_constructions == 1,
		              label + " calls prompt submitter factory once") &&
		       Expect(context.prompt_submissions == 0, label + " submits no prompt") &&
		       Expect(context.auth_token_calls == 0,
		              label + " falls back to standard PAM prompt") &&
		       Expect(ChildReaped(child_pid), label + " reaps child");
	}

	auto TestNativeSetupWithoutInputFallback(bool available_result, int install_result,
	                                         const std::string &label) -> bool {
		FakeContext      context;
		NativePamFixture fixture(&context);
		if (!Expect(fixture.Start(false), label + " starts PAM handle")) {
			return false;
		}
		context.native_available      = available_result;
		context.native_install_result = install_result;
		const pid_t child_pid         = SpawnChild(static_cast<int>(CompareExit::kTimeoutReached));
		if (!Expect(child_pid > 0, label + " child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;

		PromptCoordinator coordinator(fixture.Pamh(), Workaround::kNative, true, false,
		                              Dependencies(&context), std::chrono::seconds(5));
		const auto        result = coordinator.Run(MakeCompareRequest());
		return Expect(result.decision == PromptCoordinatorDecision::kHowdyResult,
		              label + " returns compare result without input fallback") &&
		       Expect(context.spawn_calls == 1 && context.spawned_pid == child_pid,
		              label + " spawns child once") &&
		       Expect(context.wait_calls == 1 && context.waited_pid == child_pid,
		              label + " waits for spawned child") &&
		       Expect(context.preflight_calls == 0, label + " skips input preflight") &&
		       Expect(context.auth_token_calls == 0, label + " does not request token") &&
		       Expect(context.terminate_calls == 0, label + " does not terminate child") &&
		       Expect(context.original_conversation_calls == 0,
		              label + " does not invoke original conversation") &&
		       Expect(ChildReaped(child_pid), label + " reaps child");
	}

	auto TestNativeInputSuccessUsesNativePath() -> bool {
		FakeContext      context;
		NativePamFixture fixture(&context);
		if (!Expect(fixture.Start(false), "native-input success starts PAM handle")) {
			return false;
		}
		const pid_t child_pid = SpawnBlockedChild();
		if (!Expect(child_pid > 0, "native-input success child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;

		PromptCoordinator coordinator(fixture.Pamh(), Workaround::kNativeInput, true, false,
		                              Dependencies(&context), std::chrono::seconds(5));
		const auto        result = coordinator.Run(MakeCompareRequest());
		return Expect(result.decision == PromptCoordinatorDecision::kPamResult,
		              "native-input success uses native password task") &&
		       Expect(result.pam_status == PAM_SUCCESS,
		              "native-input success preserves PAM success") &&
		       Expect(context.spawn_calls == 1 && context.spawned_pid == child_pid,
		              "native-input success spawns child once") &&
		       Expect(context.wait_calls == 1 && context.waited_pid == child_pid,
		              "native-input success waits for spawned child") &&
		       Expect(context.preflight_calls == 0, "native-input success skips input preflight") &&
		       Expect(context.auth_token_calls == 1, "native-input success requests token once") &&
		       Expect(context.terminate_calls == 1 && context.terminated_pid == child_pid,
		              "native-input success terminates blocked compare child once") &&
		       Expect(context.original_conversation_calls == 0,
		              "native-input success does not invoke original conversation") &&
		       Expect(ChildReaped(child_pid), "native-input success reaps compare child");
	}

	auto TestNativeInputSetupFallback(bool available_result, int install_result,
	                                  const std::string &label) -> bool {
		FakeContext context{
		    .token_delay  = std::chrono::milliseconds(100),
		    .token_result = PAM_SUCCESS,
		};
		NativePamFixture fixture(&context);
		if (!Expect(fixture.Start(false), label + " starts PAM handle")) {
			return false;
		}
		context.native_available      = available_result;
		context.native_install_result = install_result;
		const pid_t child_pid         = SpawnChild(static_cast<int>(CompareExit::kTimeoutReached));
		if (!Expect(child_pid > 0, label + " child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;

		PromptCoordinator coordinator(fixture.Pamh(), Workaround::kNativeInput, true, false,
		                              Dependencies(&context), std::chrono::seconds(5));
		const auto        result = coordinator.Run(MakeCompareRequest());
		return Expect(result.decision == PromptCoordinatorDecision::kPasswordFallback,
		              label + " falls back to input password task") &&
		       Expect(context.spawn_calls == 1 && context.spawned_pid == child_pid,
		              label + " spawns child once") &&
		       Expect(context.wait_calls == 1 && context.waited_pid == child_pid,
		              label + " waits for spawned child") &&
		       Expect(context.preflight_calls == 1, label + " runs input preflight once") &&
		       Expect(context.auth_token_calls == 1, label + " requests token once") &&
		       Expect(context.terminate_calls == 0, label + " does not terminate child") &&
		       Expect(context.original_conversation_calls == 0,
		              label + " does not invoke original conversation") &&
		       Expect(ChildReaped(child_pid), label + " reaps child");
	}

	auto TestNativeBlockedPromptCleanup() -> bool {
		FakeContext context{
		    .token_result          = PAM_AUTHTOK_ERR,
		    .request_native_prompt = true,
		};
		context.run_thread = std::this_thread::get_id();
		NativePamFixture fixture(&context);
		if (!Expect(fixture.Start(true), "blocked native prompt starts PAM PTY")) {
			return false;
		}
		const pid_t child_pid = SpawnChild(EXIT_SUCCESS);
		if (!Expect(child_pid > 0, "blocked native prompt child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;

		howdy::pam::PromptCoordinatorResult result;
		{
			PromptCoordinator coordinator(fixture.Pamh(), Workaround::kNative, true, false,
			                              Dependencies(&context), std::chrono::seconds(5));
			result = coordinator.Run(MakeCompareRequest());
		}

		return Expect(result.decision == PromptCoordinatorDecision::kHowdyResult,
		              "blocked native prompt returns compare result") &&
		       Expect(context.spawn_calls == 1 && context.spawned_pid == child_pid,
		              "blocked native prompt spawns child once") &&
		       Expect(context.wait_calls == 1 && context.waited_pid == child_pid,
		              "blocked native prompt waits for spawned child") &&
		       Expect(result.compare_status == 0,
		              "blocked native prompt preserves successful compare status") &&
		       Expect(result.pam_status == PAM_AUTHTOK_ERR,
		              "blocked native prompt preserves mapped cancellation status") &&
		       Expect(!context.native_terminal_restore_failed,
		              "blocked native prompt has no terminal restore failure") &&
		       Expect(context.native_prompt_seen,
		              "blocked native prompt reaches native conversation") &&
		       Expect(context.auth_token_calls == 1, "blocked native prompt requests token once") &&
		       Expect(context.terminate_calls == 0,
		              "blocked native prompt does not terminate compare child") &&
		       Expect(context.original_conversation_calls == 0,
		              "blocked native prompt bypasses original conversation") &&
		       Expect(context.native_abort_calls == 1,
		              "blocked native prompt requests one abort") &&
		       Expect(context.native_create_thread == context.run_thread &&
		                  context.native_install_thread == context.run_thread &&
		                  context.auth_token_thread == context.run_thread &&
		                  context.native_restore_thread == context.run_thread,
		              "native PAM operations stay on run caller thread") &&
		       Expect(context.native_abort_thread == context.wait_thread &&
		                  context.native_abort_thread != context.run_thread,
		              "native abort runs on compare worker") &&
		       Expect(context.native_restore_calls == 1, "blocked native prompt restores once") &&
		       Expect(ChildReaped(child_pid), "blocked native prompt reaps child");
	}

	auto TestNativePamWins() -> bool {
		FakeContext context{
		    .request_native_prompt  = true,
		    .complete_native_prompt = true,
		};
		NativePamFixture fixture(&context);
		if (!Expect(fixture.Start(true), "native PAM winner starts PAM PTY")) {
			return false;
		}
		const pid_t child_pid = SpawnBlockedChild();
		if (!Expect(child_pid > 0, "native PAM winner child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;

		howdy::pam::PromptCoordinatorResult result;
		{
			PromptCoordinator coordinator(fixture.Pamh(), Workaround::kNative, true, false,
			                              Dependencies(&context), std::chrono::seconds(5));
			result = coordinator.Run(MakeCompareRequest());
		}
		return Expect(result.decision == PromptCoordinatorDecision::kPamResult,
		              "native PAM winner returns PAM result") &&
		       Expect(context.spawn_calls == 1 && context.spawned_pid == child_pid,
		              "native PAM winner spawns child once") &&
		       Expect(context.wait_calls == 1 && context.waited_pid == child_pid,
		              "native PAM winner waits for spawned child") &&
		       Expect(result.pam_status == PAM_SUCCESS,
		              "native PAM winner preserves PAM success") &&
		       Expect(context.native_prompt_installed,
		              "native PAM winner observes installed native conversation") &&
		       Expect(context.native_prompt_seen,
		              "native PAM winner observes native password prompt") &&
		       Expect(context.native_prompt_input_sent,
		              "native PAM winner sends password input after prompt observation") &&
		       Expect(context.native_prompt_completed,
		              "native PAM winner completes native conversation") &&
		       Expect(!context.native_terminal_restore_failed,
		              "native PAM winner records no terminal restore failure") &&
		       Expect(context.pam_completion_observed_by_waiter,
		              "native PAM winner synchronizes completion before compare wait") &&
		       Expect(context.auth_token_calls == 1,
		              "native PAM winner completes password task once") &&
		       Expect(context.preflight_calls == 0, "native PAM winner skips input preflight") &&
		       Expect(context.terminate_calls == 1 && context.terminated_pid == child_pid,
		              "native PAM winner terminates blocked compare child once") &&
		       Expect(ChildReaped(child_pid), "native PAM winner reaps compare child") &&
		       Expect(context.native_abort_calls == 0,
		              "native PAM winner needs no abort after password_call_returned") &&
		       Expect(context.native_restore_calls == 1,
		              "native PAM winner restores native prompt once") &&
		       Expect(context.original_conversation_calls == 0,
		              "native PAM winner leaves no blocked native prompt task");
	}

	auto TestNativeTerminalRestoreFailureForcesSystemError() -> bool {
		FakeContext context{
		    .token_result                             = PAM_AUTHTOK_ERR,
		    .request_native_prompt                    = true,
		    .native_terminal_restore_failure_on_abort = true,
		};
		NativePamFixture fixture(&context);
		if (!Expect(fixture.Start(true), "native terminal restore failure starts PAM PTY")) {
			return false;
		}
		const pid_t child_pid = SpawnChild(EXIT_SUCCESS);
		if (!Expect(child_pid > 0, "native terminal restore failure child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;

		PromptCoordinator coordinator(fixture.Pamh(), Workaround::kNative, true, false,
		                              Dependencies(&context), std::chrono::seconds(5));
		const auto        result = coordinator.Run(MakeCompareRequest());
		return Expect(result.decision == PromptCoordinatorDecision::kPamResult,
		              "native terminal restore failure overrides successful face result") &&
		       Expect(result.pam_status == PAM_SYSTEM_ERR,
		              "native terminal restore failure returns PAM_SYSTEM_ERR") &&
		       Expect(context.native_terminal_restore_failed,
		              "native terminal restore failure is explicitly reported after abort") &&
		       Expect(context.native_prompt_seen,
		              "native terminal restore failure reaches active native prompt") &&
		       Expect(
		           context.native_abort_calls == 1,
		           "native terminal restore failure aborts native prompt after compare success") &&
		       Expect(context.native_restore_calls == 1,
		              "native terminal restore failure restores PAM conversation once") &&
		       Expect(ChildReaped(child_pid), "native terminal restore failure reaps child");
	}

	auto
	TestNativeRestoreFailureForcesSystemError(howdy::pam::ConversationRestoreResult restore_result,
	                                          const std::string &message) -> bool {
		FakeContext context{
		    .request_native_prompt  = true,
		    .complete_native_prompt = true,
		    .native_restore_result  = restore_result,
		};
		NativePamFixture fixture(&context);
		if (!Expect(fixture.Start(true), message + ": starts PAM PTY")) {
			return false;
		}
		const pid_t child_pid = SpawnBlockedChild();
		if (!Expect(child_pid > 0, message + ": child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;

		PromptCoordinator coordinator(fixture.Pamh(), Workaround::kNative, true, false,
		                              Dependencies(&context), std::chrono::seconds(5));
		const auto        result = coordinator.Run(MakeCompareRequest());
		return Expect(result.decision == PromptCoordinatorDecision::kPamResult,
		              message + ": restoration failure overrides normal winner") &&
		       Expect(result.pam_status == PAM_SYSTEM_ERR,
		              message + ": restoration failure returns hard PAM error") &&
		       Expect(context.native_restore_calls == 1,
		              message + ": restoration attempted once") &&
		       Expect(ChildReaped(child_pid), message + ": child reaped");
	}
}  // namespace

auto RunPromptModeTests() -> bool {
	bool ok = true;
	ok &= TestExistingAuthTokenSkipsPromptWorkaround();
	ok &= TestSecretObservationSetupFallback(0, "null secret observer");
	ok &= TestSecretObservationSetupFallback(1, "unavailable secret observer");
	ok &= TestSecretObservationSetupFallback(2, "secret observer install failure");
	ok &= TestSecretObservationSetupFallback(3, "secret observer setup exception");
	ok &= TestSecretObservationSetupFallback(4, "secret observer unknown exception");
	ok &= TestNativePromptSetupExceptionFallback(false, "native setup exception");
	ok &= TestNativePromptSetupExceptionFallback(true, "native setup unknown exception");
	ok &= TestAuthTokenExceptionMapping(false, "auth token exception");
	ok &= TestAuthTokenExceptionMapping(true, "auth token unknown exception");
	ok &= TestSecretRestoreFailureMapping();
	ok &= TestInputSuccessSubmitsPrompt();
	ok &= TestCompareFailurePasswordResult(PAM_SUCCESS, "successful password fallback");
	ok &= TestCompareFailurePasswordResult(PAM_CONV_ERR, "failed password fallback");
	ok &= TestCompareSignalPasswordFallback();
	ok &= TestInputPreflightFallback();
	ok &= TestPromptSubmitterConstructionFallback(false, "prompt submitter construction failure");
	ok &= TestPromptSubmitterConstructionFallback(true, "null prompt submitter factory result");
	ok &= TestNativeSetupWithoutInputFallback(false, -1, "native unavailable");
	ok &= TestNativeSetupWithoutInputFallback(true, PAM_CONV_ERR, "native install failure");
	ok &= TestNativeInputSuccessUsesNativePath();
	ok &= TestNativeInputSetupFallback(false, -1, "native-input unavailable");
	ok &= TestNativeInputSetupFallback(true, PAM_CONV_ERR, "native-input install failure");
	ok &= TestNativeBlockedPromptCleanup();
	ok &= TestNativePamWins();
	ok &= TestNativeTerminalRestoreFailureForcesSystemError();
	ok &= TestNativeRestoreFailureForcesSystemError(
	    howdy::pam::ConversationRestoreResult::kFailClosedInstalled,
	    "fail-closed callback installation");
	ok &= TestNativeRestoreFailureForcesSystemError(howdy::pam::ConversationRestoreResult::kUnsafe,
	                                                "unsafe native restoration");
	return ok;
}
