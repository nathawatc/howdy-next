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
		if (!expect(child_pid > 0, label + " child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;

		PromptCoordinator coordinator(nullptr, Workaround::kOff, false, false,
		                              Dependencies(&context), std::chrono::seconds(5));
		const auto        result = coordinator.Run(request);
		return expect(result.decision == PromptCoordinatorDecision::kHowdyResult,
		              label + " returns Howdy result") &&
		       expect(context.spawn_calls == 1 && context.spawned_pid == child_pid,
		              label + " spawns child once") &&
		       expect(context.wait_calls == 1 && context.waited_pid == child_pid,
		              label + " waits for spawned child") &&
		       expect(context.spawned_config_path == expected_config_path,
		              label + " preserves config path") &&
		       expect(context.spawned_username == expected_username,
		              label + " preserves username") &&
		       expect(context.spawned_user_models_dir == expected_user_models_dir,
		              label + " preserves models directory") &&
		       expect(context.spawned_staged_runtime == expected_staged_runtime,
		              label + " preserves staged-runtime selection") &&
		       expect(ChildReaped(child_pid), label + " reaps child");
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
		                              Dependencies(&context), std::chrono::seconds(5));

		const auto result = coordinator.Run(MakeCompareRequest());
		return expect(result.decision == PromptCoordinatorDecision::kCompareSpawnFailed,
		              "spawn failure returns compare-spawn-failed result") &&
		       expect(GetCallbackCounts(context) == CallbackCounts{.spawn = 1},
		              "spawn failure invokes no downstream callbacks") &&
		       expect(context.spawned_pid == -1, "spawn failure creates no child task");
	}

	auto TestInvalidSpawnPid() -> bool {
		FakeContext context;
		context.next_child_pid = -1;
		PromptCoordinator coordinator(nullptr, Workaround::kInput, true, false,
		                              Dependencies(&context), std::chrono::seconds(5));

		const auto result = coordinator.Run(MakeCompareRequest());
		return expect(result.decision == PromptCoordinatorDecision::kCompareSpawnFailed,
		              "invalid spawn PID returns compare-spawn-failed result") &&
		       expect(GetCallbackCounts(context) == CallbackCounts{.spawn = 1},
		              "invalid spawn PID invokes no downstream callbacks");
	}

	auto TestOneShotAfterSpawnFailure() -> bool {
		FakeContext       context{.spawn_result = EACCES};
		PromptCoordinator coordinator(nullptr, Workaround::kInput, true, false,
		                              Dependencies(&context), std::chrono::seconds(5));

		const auto first  = coordinator.Run(MakeCompareRequest());
		const auto second = coordinator.Run(
		    MakeCompareRequest("/different/config.ini", "bob", "/different/models", true));
		return expect(first.decision == PromptCoordinatorDecision::kCompareSpawnFailed,
		              "spawn-failure one-shot first run reports spawn failure") &&
		       expect(second.decision == PromptCoordinatorDecision::kAlreadyRun,
		              "spawn-failure one-shot second run is rejected") &&
		       expect(GetCallbackCounts(context) == CallbackCounts{.spawn = 1},
		              "spawn-failure one-shot invokes spawn only once");
	}

	auto TestInvalidDependencies() -> bool {
		bool ok = true;
		for (int missing = 0; missing < 7; ++missing) {
			FakeContext context;
			auto        deps = Dependencies(&context);
			switch (missing) {
				case 0:
					deps.spawn_compare_process = nullptr;
					break;
				case 1:
					deps.wait_for_compare_process = nullptr;
					break;
				case 2:
					deps.cancel_and_reap_compare_process = nullptr;
					break;
				case 3:
					deps.input_prompt_preflight = nullptr;
					break;
				case 4:
					deps.create_prompt_submitter = nullptr;
					break;
				case 5:
					deps.create_native_prompt = nullptr;
					break;
				case 6:
					deps.request_auth_token = nullptr;
					break;
				default:
					break;
			}

			PromptCoordinator coordinator(nullptr, Workaround::kInput, true, false, deps,
			                              std::chrono::seconds(5));
			ok &= expect(!coordinator.Valid(), "missing dependency is invalid");
			const auto before = GetCallbackCounts(context);
			const auto result = coordinator.Run(MakeCompareRequest());
			ok &= expect(result.decision == PromptCoordinatorDecision::kInvalidDependencies,
			             "invalid coordinator returns invalid-dependencies result");
			ok &= expect(GetCallbackCounts(context) == before,
			             "invalid coordinator invokes no callback");
		}
		return ok;
	}

	auto TestOneShot() -> bool {
		FakeContext context;
		const pid_t child_pid = SpawnChild(EXIT_SUCCESS);
		if (!expect(child_pid > 0, "one-shot child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;

		PromptCoordinator coordinator(nullptr, Workaround::kOff, false, false,
		                              Dependencies(&context), std::chrono::seconds(5));
		const auto        first = coordinator.Run(MakeCompareRequest());
		if (!expect(first.decision == PromptCoordinatorDecision::kHowdyResult,
		            "one-shot first run succeeds")) {
			return false;
		}
		const auto before = GetCallbackCounts(context);
		const auto second = coordinator.Run(MakeCompareRequest("/different/config.ini"));
		return expect(second.decision == PromptCoordinatorDecision::kAlreadyRun,
		              "one-shot second run is rejected") &&
		       expect(context.spawn_calls == 1 && context.spawned_pid == child_pid,
		              "one-shot first run spawns child once") &&
		       expect(context.wait_calls == 1 && context.waited_pid == child_pid,
		              "one-shot first run waits for spawned child") &&
		       expect(GetCallbackCounts(context) == before,
		              "one-shot second run invokes no callback") &&
		       expect(ChildReaped(child_pid), "one-shot first run reaps child");
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
