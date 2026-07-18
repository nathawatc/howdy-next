#include "prompt/prompt_coordinator_test_groups.hpp"
#include "prompt/prompt_coordinator_test_support.hpp"

namespace {
	using namespace howdy::test::prompt_coordinator;

	auto test_watchdog_timeout_reaps_blocked_child() -> bool {
		const pid_t child_pid = spawn_blocked_child();
		if (!expect(child_pid > 0, "watchdog timeout child spawned")) {
			return false;
		}

		const int status = howdy::pam::compare_process::wait_until(
		    child_pid, std::chrono::steady_clock::now() + 40ms);
		return expect(status == timeout_wait_status(),
		              "watchdog timeout returns synthetic timeout status") &&
		       expect(child_reaped(child_pid), "watchdog timeout reaps blocked child");
	}

	auto test_watchdog_kills_sigterm_ignoring_child() -> bool {
		const pid_t child_pid = spawn_sigterm_ignoring_child();
		if (!expect(child_pid > 0, "SIGTERM-ignoring watchdog child spawned")) {
			return false;
		}

		const int status = howdy::pam::compare_process::wait_until(
		    child_pid, std::chrono::steady_clock::now() + 40ms);
		return expect(status == timeout_wait_status(),
		              "SIGTERM-ignoring child returns synthetic timeout status") &&
		       expect(child_reaped(child_pid), "SIGTERM-ignoring child is SIGKILLed and reaped");
	}

	auto test_watchdog_preserves_natural_exit_status() -> bool {
		const pid_t child_pid = spawn_child(17, 10ms);
		if (!expect(child_pid > 0, "natural watchdog child spawned")) {
			return false;
		}

		const int status = howdy::pam::compare_process::wait_until(
		    child_pid, std::chrono::steady_clock::now() + 1s);
		return expect(status == (17 << 8), "watchdog preserves natural exit wait status") &&
		       expect(child_reaped(child_pid), "watchdog reaps naturally exited child");
	}

	auto test_watchdog_timeout_keeps_password_fallback() -> bool {
		FakeContext context{
		    .token_result = PAM_SUCCESS,
		    .token_delay  = 100ms,
		};
		const pid_t child_pid = spawn_blocked_child();
		if (!expect(child_pid > 0, "watchdog fallback child spawned")) {
			return false;
		}
		context.next_child_pid        = child_pid;
		auto deps                     = dependencies(&context);
		deps.wait_for_compare_process = watchdog_wait_for_compare;

		PromptCoordinator coordinator(nullptr, Workaround::Input, true, false, deps, 40ms);
		const auto        result = coordinator.run(make_compare_request());
		return expect(result.decision == PromptCoordinatorDecision::kPasswordFallback,
		              "watchdog timeout keeps password fallback") &&
		       expect(result.compare_status == timeout_wait_status(),
		              "password fallback preserves watchdog timeout status") &&
		       expect(result.pam_status == PAM_SUCCESS,
		              "password fallback preserves PAM success") &&
		       expect(child_reaped(child_pid), "password fallback reaps watchdog child");
	}

	auto test_pam_success_reaps_before_watchdog() -> bool {
		FakeContext context;
		const pid_t child_pid = spawn_blocked_child();
		if (!expect(child_pid > 0, "PAM-before-watchdog child spawned")) {
			return false;
		}
		context.next_child_pid        = child_pid;
		auto deps                     = dependencies(&context);
		deps.wait_for_compare_process = watchdog_wait_for_compare;

		PromptCoordinator coordinator(nullptr, Workaround::Input, true, false, deps, 1s);
		const auto        result = coordinator.run(make_compare_request());
		return expect(result.decision == PromptCoordinatorDecision::kPamResult,
		              "PAM success wins before watchdog") &&
		       expect(context.terminate_calls == 1, "PAM success terminates compare child") &&
		       expect(context.last_wait_status != timeout_wait_status(),
		              "PAM success does not use watchdog timeout status") &&
		       expect(child_reaped(child_pid), "PAM success reaps compare child");
	}

	auto test_invalid_hard_timeout_fails_closed() -> bool {
		bool ok = true;
		for (const auto timeout : {std::chrono::milliseconds::zero(), -1ms}) {
			FakeContext       context;
			PromptCoordinator coordinator(nullptr, Workaround::Input, true, false,
			                              dependencies(&context), timeout);
			const auto        result = coordinator.run(make_compare_request());
			ok &= expect(!coordinator.valid(), "nonpositive hard timeout is invalid");
			ok &= expect(result.decision == PromptCoordinatorDecision::kInvalidDependencies,
			             "nonpositive hard timeout fails closed");
			ok &= expect(callback_counts(context) == CallbackCounts{},
			             "nonpositive hard timeout starts no callbacks");
		}
		return ok;
	}

	auto test_compare_wins_without_password_prompt() -> bool {
		FakeContext context;
		const pid_t child_pid = spawn_child(EXIT_SUCCESS);
		if (!expect(child_pid > 0, "compare-winner child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;

		PromptCoordinator coordinator(nullptr, Workaround::Off, false, false,
		                              dependencies(&context), std::chrono::seconds(5));
		const auto        result = coordinator.run(make_compare_request());
		const bool        reaped = child_reaped(child_pid);
		return expect(result.decision == PromptCoordinatorDecision::kHowdyResult,
		              "compare winner returns Howdy result") &&
		       expect(context.spawn_calls == 1 && context.spawned_pid == child_pid,
		              "compare winner spawns child once") &&
		       expect(result.compare_status == 0,
		              "compare winner preserves exact successful wait status") &&
		       expect(context.wait_calls == 1 && context.waited_pid == child_pid,
		              "compare winner waits once for child") &&
		       expect(context.auth_token_calls == 0,
		              "compare winner does not request disabled password") &&
		       expect(context.terminate_calls == 0, "compare winner does not terminate child") &&
		       expect(reaped, "compare winner reaps child");
	}

	auto test_pam_wins() -> bool {
		FakeContext context;
		const pid_t child_pid = spawn_child(EXIT_SUCCESS, std::chrono::seconds(2));
		if (!expect(child_pid > 0, "PAM-winner child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;

		PromptCoordinator coordinator(nullptr, Workaround::Input, true, false,
		                              dependencies(&context), std::chrono::seconds(5));
		int               input_failure_calls = 0;
		const auto result = coordinator.run(make_compare_request(), [&input_failure_calls] -> void {
			++input_failure_calls;
		});
		const bool reaped = child_reaped(child_pid);
		return expect(result.decision == PromptCoordinatorDecision::kPamResult,
		              "PAM winner returns PAM result") &&
		       expect(context.spawn_calls == 1 && context.spawned_pid == child_pid,
		              "PAM winner spawns child once") &&
		       expect(context.wait_calls == 1 && context.waited_pid == child_pid,
		              "PAM winner waits for spawned child") &&
		       expect(result.pam_status == PAM_SUCCESS, "PAM winner preserves PAM success") &&
		       expect(context.preflight_calls == 1, "PAM winner runs input preflight once") &&
		       expect(context.auth_token_calls == 1, "PAM winner requests token once") &&
		       expect(input_failure_calls == 0,
		              "successful input workaround reports no input failure") &&
		       expect(context.terminate_calls == 1 && context.terminated_pid == child_pid,
		              "PAM winner terminates compare child once") &&
		       expect(reaped, "PAM winner reaps compare child");
	}
}  // namespace

auto main() -> int {
	bool ok = true;
	ok &= test_watchdog_timeout_reaps_blocked_child();
	ok &= test_watchdog_kills_sigterm_ignoring_child();
	ok &= test_watchdog_preserves_natural_exit_status();
	ok &= test_watchdog_timeout_keeps_password_fallback();
	ok &= test_pam_success_reaps_before_watchdog();
	ok &= test_invalid_hard_timeout_fails_closed();
	ok &= test_compare_wins_without_password_prompt();
	ok &= test_pam_wins();
	ok &= run_prompt_mode_tests();
	ok &= run_prompt_adapter_tests();
	return ok ? 0 : 1;
}
