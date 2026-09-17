#include "prompt/prompt_coordinator_fake.hpp"
#include "prompt/prompt_coordinator_test_groups.hpp"
#include "support/process_test_support.hpp"

namespace {
	using namespace howdy::test::process;
	using namespace howdy::test::prompt_coordinator;

	auto TestLaunchRequest(const howdy::pam::CompareLaunchRequest &request,
	                       const std::string                      &expected_config_path,
	                       const std::string                      &expected_username,
	                       const std::string                      &expected_user_models_dir,
	                       bool expected_staged_runtime, const std::string &label) -> bool {
		FakeContext context;
		const pid_t child_pid = SpawnChild(EXIT_SUCCESS);
		if (!Expect(child_pid > 0, label + " child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;

		PromptCoordinator coordinator(nullptr, Workaround::kOff, false, false, Operations(&context),
		                              Timeout());
		const auto        result = coordinator.Run(request);
		return Expect(result.decision == PromptCoordinatorDecision::kHowdyResult,
		              label + " returns Howdy result") &&
		       Expect(context.spawn_calls == 1 && context.spawned_pid == child_pid,
		              label + " spawns child once") &&
		       Expect(context.wait_calls == 1 && context.waited_pid == child_pid,
		              label + " waits for spawned child") &&
		       Expect(context.spawned_config_path == expected_config_path,
		              label + " preserves config path") &&
		       Expect(context.spawned_username == expected_username,
		              label + " preserves username") &&
		       Expect(context.spawned_user_models_dir == expected_user_models_dir,
		              label + " preserves models directory") &&
		       Expect(context.spawned_staged_runtime == expected_staged_runtime,
		              label + " preserves staged-runtime selection") &&
		       Expect(ChildReaped(child_pid), label + " reaps child");
	}

	auto TestDirectRuntimeLaunchRequest() -> bool {
		return TestLaunchRequest(
		    MakeCompareRequest("/etc/howdy/config.ini", "alice", "/etc/howdy/models", false),
		    "/etc/howdy/config.ini", "alice", "/etc/howdy/models", false, "direct runtime request");
	}

	auto TestStagedRuntimeLaunchRequest() -> bool {
		return TestLaunchRequest(MakeCompareRequest("/run/howdy/runtime/config.ini", "alice",
		                                            "/run/howdy/runtime/models", true),
		                         "/run/howdy/runtime/config.ini", "alice",
		                         "/run/howdy/runtime/models", true, "staged runtime request");
	}

	auto TestSpawnFailure() -> bool {
		FakeContext       context{.spawn_result = EACCES};
		PromptCoordinator coordinator(nullptr, Workaround::kInput, true, false,
		                              Operations(&context), Timeout());

		const auto result = coordinator.Run(MakeCompareRequest());
		return Expect(result.decision == PromptCoordinatorDecision::kCompareSpawnFailed,
		              "spawn failure returns compare-spawn-failed result") &&
		       Expect(GetCallbackCounts(context) == CallbackCounts{.spawn = 1},
		              "spawn failure invokes no downstream callbacks") &&
		       Expect(context.spawned_pid == -1, "spawn failure creates no child task");
	}

	auto TestInvalidSpawnPid() -> bool {
		FakeContext context;
		context.next_child_pid = -1;
		PromptCoordinator coordinator(nullptr, Workaround::kInput, true, false,
		                              Operations(&context), Timeout());

		const auto result = coordinator.Run(MakeCompareRequest());
		return Expect(result.decision == PromptCoordinatorDecision::kCompareSpawnFailed,
		              "invalid spawn PID returns compare-spawn-failed result") &&
		       Expect(GetCallbackCounts(context) == CallbackCounts{.spawn = 1},
		              "invalid spawn PID invokes no downstream callbacks");
	}

	auto TestOneShotAfterSpawnFailure() -> bool {
		FakeContext       context{.spawn_result = EACCES};
		PromptCoordinator coordinator(nullptr, Workaround::kInput, true, false,
		                              Operations(&context), Timeout());

		const auto first  = coordinator.Run(MakeCompareRequest());
		const auto second = coordinator.Run(
		    MakeCompareRequest("/different/config.ini", "bob", "/different/models", true));
		return Expect(first.decision == PromptCoordinatorDecision::kCompareSpawnFailed,
		              "spawn-failure one-shot first run reports spawn failure") &&
		       Expect(second.decision == PromptCoordinatorDecision::kAlreadyRun,
		              "spawn-failure one-shot second run is rejected") &&
		       Expect(GetCallbackCounts(context) == CallbackCounts{.spawn = 1},
		              "spawn-failure one-shot invokes spawn only once");
	}

	auto TestInvalidDependencies() -> bool {
		bool ok = true;
		for (int missing = 0; missing < 8; ++missing) {
			FakeContext context;
			const auto  spawn     = missing == 0 ? nullptr : SpawnCompareProcess;
			const auto  wait      = missing == 1 ? nullptr : WaitForCompare;
			const auto  cancel    = missing == 2 ? nullptr : CancelAndReapCompare;
			const auto  preflight = missing == 3 ? nullptr : InputPreflight;
			const auto  submitter = missing == 4 ? nullptr : CreatePromptSubmitter;
			const auto  native    = missing == 5 ? nullptr : CreateNativePrompt;
			const auto  secret    = missing == 6 ? nullptr : CreateSecretPromptConversation;
			const auto  token     = missing == 7 ? nullptr : RequestAuthToken;

			const auto ops = PromptCoordinatorOperations::Create(
			    &context, spawn, wait, cancel, preflight, submitter, native, secret, token);
			ok &= Expect(!ops.has_value(), "missing callback is rejected by Create");
		}
		return ok;
	}

	auto TestOneShot() -> bool {
		FakeContext context;
		const pid_t child_pid = SpawnChild(EXIT_SUCCESS);
		if (!Expect(child_pid > 0, "one-shot child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;

		PromptCoordinator coordinator(nullptr, Workaround::kOff, false, false, Operations(&context),
		                              Timeout());
		const auto        first = coordinator.Run(MakeCompareRequest());
		if (!Expect(first.decision == PromptCoordinatorDecision::kHowdyResult,
		            "one-shot first run succeeds")) {
			return false;
		}
		const auto before = GetCallbackCounts(context);
		const auto second = coordinator.Run(MakeCompareRequest("/different/config.ini"));
		return Expect(second.decision == PromptCoordinatorDecision::kAlreadyRun,
		              "one-shot second run is rejected") &&
		       Expect(context.spawn_calls == 1 && context.spawned_pid == child_pid,
		              "one-shot first run spawns child once") &&
		       Expect(context.wait_calls == 1 && context.waited_pid == child_pid,
		              "one-shot first run waits for spawned child") &&
		       Expect(GetCallbackCounts(context) == before,
		              "one-shot second run invokes no callback") &&
		       Expect(ChildReaped(child_pid), "one-shot first run reaps child");
	}

}  // namespace

auto RunPromptAdapterTests() -> bool {
	bool ok = true;
	ok &= TestDirectRuntimeLaunchRequest();
	ok &= TestStagedRuntimeLaunchRequest();

	ok &= TestSpawnFailure();
	ok &= TestInvalidSpawnPid();
	ok &= TestOneShotAfterSpawnFailure();
	ok &= TestInvalidDependencies();
	ok &= TestOneShot();
	return ok;
}
