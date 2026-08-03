#include "paths.hpp"
#include "prompt/prompt_coordinator_test_groups.hpp"
#include "prompt/prompt_coordinator_test_support.hpp"

namespace {
	using namespace howdy::test::prompt_coordinator;

	auto test_launch_request(const howdy::pam::CompareLaunchRequest &request,
	                         const std::string                      &expected_config_path,
	                         const std::string                      &expected_username,
	                         const std::string                      &expected_user_models_dir,
	                         bool expected_staged_runtime, const std::string &label) -> bool {
		FakeContext context;
		const pid_t child_pid = spawn_child(EXIT_SUCCESS);
		if (!expect(child_pid > 0, label + " child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;

		PromptCoordinator coordinator(nullptr, Workaround::kOff, false, false,
		                              dependencies(&context), std::chrono::seconds(5));
		const auto        result = coordinator.run(request);
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
		       expect(child_reaped(child_pid), label + " reaps child");
	}

	auto test_direct_runtime_launch_request() -> bool {
		return test_launch_request(
		    make_compare_request("/etc/howdy/config.ini", "alice", "/etc/howdy/models", false),
		    "/etc/howdy/config.ini", "alice", "/etc/howdy/models", false, "direct runtime request");
	}

	auto test_staged_runtime_launch_request() -> bool {
		return test_launch_request(make_compare_request("/run/howdy/runtime/config.ini", "alice",
		                                                "/run/howdy/runtime/models", true),
		                           "/run/howdy/runtime/config.ini", "alice",
		                           "/run/howdy/runtime/models", true, "staged runtime request");
	}

	auto test_production_spawn_adapter(const howdy::pam::CompareLaunchRequest &request,
	                                   const std::vector<std::string>         &expected_argv,
	                                   const std::vector<std::string>         &expected_environment,
	                                   const std::string                      &label) -> bool {
		PosixSpawnCapture capture;
		pid_t             child_pid = -1;
		const int result = howdy::pam::compare_process::spawn(request, &child_pid,
		                                                      posix_spawn_operations(&capture));

		return expect(result == 0, label + " returns spawn success") &&
		       expect(capture.init_calls == 1, label + " initializes file actions once") &&
		       expect(capture.addclosefrom_calls == 1, label + " adds close-from action once") &&
		       expect(capture.closefrom_fd == STDERR_FILENO + 1,
		              label + " closes descriptors beginning at 3") &&
		       expect(capture.spawn_calls == 1, label + " calls posix_spawn once") &&
		       expect(capture.spawn_actions != nullptr,
		              label + " passes non-null file actions to spawn") &&
		       expect(capture.spawn_actions == capture.initialized_actions &&
		                  capture.spawn_actions == capture.closefrom_actions,
		              label + " passes initialized close-from actions to spawn") &&
		       expect(capture.destroy_calls == 1, label + " destroys file actions once") &&
		       expect(capture.destroyed_actions == capture.initialized_actions,
		              label + " destroys initialized file actions") &&
		       expect(child_pid == capture.next_pid, label + " preserves spawned PID") &&
		       expect(capture.path == kCompareProcessPath, label + " preserves executable path") &&
		       expect(capture.argv == expected_argv, label + " preserves exact argv") &&
		       expect(capture.environment == expected_environment,
		              label + " preserves exact environment");
	}

	auto test_production_direct_runtime_environment() -> bool {
		return test_production_spawn_adapter(
		    make_compare_request("/etc/howdy/config.ini", "alice", "/etc/howdy/models", false),
		    {kCompareProcessPath, "--config", "/etc/howdy/config.ini", "alice"}, {},
		    "production direct runtime");
	}

	auto test_production_staged_runtime_environment() -> bool {
		return test_production_spawn_adapter(
		    make_compare_request("/run/howdy/runtime/config.ini", "alice",
		                         "/run/howdy/runtime/models", true),
		    {kCompareProcessPath, "--config", "/run/howdy/runtime/config.ini", "alice"},
		    {"HOWDY_USER_MODELS_DIR=/run/howdy/runtime/models"}, "production staged runtime");
	}

	auto test_owned_launch_request_from_temporaries() -> bool {
		const howdy::pam::CompareLaunchRequest request = {
		    .config_path     = std::string("/run/howdy/temporary/config.ini"),
		    .username        = std::string("temporary-user"),
		    .user_models_dir = std::string("/run/howdy/temporary/models"),
		    .staged_runtime  = true,
		};

		return test_production_spawn_adapter(
		    request,
		    {kCompareProcessPath, "--config", "/run/howdy/temporary/config.ini", "temporary-user"},
		    {"HOWDY_USER_MODELS_DIR=/run/howdy/temporary/models"}, "owned temporary request");
	}

	auto test_production_file_actions_init_failure() -> bool {
		PosixSpawnCapture capture;
		capture.init_result = ENOMEM;
		pid_t     child_pid = -1;
		const int result    = howdy::pam::compare_process::spawn(make_compare_request(), &child_pid,
		                                                         posix_spawn_operations(&capture));

		return expect(result == ENOMEM, "file-actions init failure preserves error") &&
		       expect(capture.init_calls == 1, "file-actions init failure initializes once") &&
		       expect(capture.addclosefrom_calls == 0,
		              "file-actions init failure does not add close-from action") &&
		       expect(capture.spawn_calls == 0, "file-actions init failure does not spawn") &&
		       expect(capture.destroy_calls == 0,
		              "file-actions init failure does not destroy uninitialized actions") &&
		       expect(child_pid == -1, "file-actions init failure leaves child PID unchanged");
	}

	auto test_production_closefrom_failure() -> bool {
		PosixSpawnCapture capture;
		capture.addclosefrom_result = EINVAL;
		pid_t     child_pid         = -1;
		const int result = howdy::pam::compare_process::spawn(make_compare_request(), &child_pid,
		                                                      posix_spawn_operations(&capture));

		return expect(result == EINVAL, "close-from setup failure preserves error") &&
		       expect(capture.init_calls == 1, "close-from setup failure initializes once") &&
		       expect(capture.addclosefrom_calls == 1,
		              "close-from setup failure adds action once") &&
		       expect(capture.closefrom_fd == STDERR_FILENO + 1,
		              "close-from setup failure begins at descriptor 3") &&
		       expect(capture.spawn_calls == 0, "close-from setup failure does not spawn") &&
		       expect(capture.destroy_calls == 1,
		              "close-from setup failure destroys initialized actions once") &&
		       expect(capture.destroyed_actions == capture.initialized_actions,
		              "close-from setup failure destroys initialized actions") &&
		       expect(child_pid == -1, "close-from setup failure leaves child PID unchanged");
	}

	auto test_production_spawn_failure() -> bool {
		PosixSpawnCapture capture;
		capture.spawn_result = EACCES;
		pid_t     child_pid  = -1;
		const int result = howdy::pam::compare_process::spawn(make_compare_request(), &child_pid,
		                                                      posix_spawn_operations(&capture));

		return expect(result == EACCES, "production spawn failure preserves error") &&
		       expect(capture.init_calls == 1, "production spawn failure initializes once") &&
		       expect(capture.addclosefrom_calls == 1,
		              "production spawn failure adds close-from action once") &&
		       expect(capture.spawn_calls == 1,
		              "production spawn failure calls posix_spawn once") &&
		       expect(capture.spawn_actions != nullptr,
		              "production spawn failure passes non-null file actions") &&
		       expect(capture.destroy_calls == 1,
		              "production spawn failure destroys file actions once") &&
		       expect(child_pid == -1, "production spawn failure leaves child PID unchanged") &&
		       expect(capture.path == kCompareProcessPath,
		              "production spawn failure preserves executable path") &&
		       expect(capture.argv == std::vector<std::string>{kCompareProcessPath, "--config",
		                                                       "/etc/howdy/config.ini", "alice"},
		              "production spawn failure preserves exact argv") &&
		       expect(capture.environment.empty(),
		              "production spawn failure preserves empty direct environment");
	}

	auto test_spawn_failure() -> bool {
		FakeContext       context{.spawn_result = EACCES};
		PromptCoordinator coordinator(nullptr, Workaround::kInput, true, false,
		                              dependencies(&context), std::chrono::seconds(5));

		const auto result = coordinator.run(make_compare_request());
		return expect(result.decision == PromptCoordinatorDecision::kCompareSpawnFailed,
		              "spawn failure returns compare-spawn-failed result") &&
		       expect(callback_counts(context) == CallbackCounts{.spawn = 1},
		              "spawn failure invokes no downstream callbacks") &&
		       expect(context.spawned_pid == -1, "spawn failure creates no child task");
	}

	auto test_invalid_spawn_pid() -> bool {
		FakeContext context;
		context.next_child_pid = -1;
		PromptCoordinator coordinator(nullptr, Workaround::kInput, true, false,
		                              dependencies(&context), std::chrono::seconds(5));

		const auto result = coordinator.run(make_compare_request());
		return expect(result.decision == PromptCoordinatorDecision::kCompareSpawnFailed,
		              "invalid spawn PID returns compare-spawn-failed result") &&
		       expect(callback_counts(context) == CallbackCounts{.spawn = 1},
		              "invalid spawn PID invokes no downstream callbacks");
	}

	auto test_one_shot_after_spawn_failure() -> bool {
		FakeContext       context{.spawn_result = EACCES};
		PromptCoordinator coordinator(nullptr, Workaround::kInput, true, false,
		                              dependencies(&context), std::chrono::seconds(5));

		const auto first  = coordinator.run(make_compare_request());
		const auto second = coordinator.run(
		    make_compare_request("/different/config.ini", "bob", "/different/models", true));
		return expect(first.decision == PromptCoordinatorDecision::kCompareSpawnFailed,
		              "spawn-failure one-shot first run reports spawn failure") &&
		       expect(second.decision == PromptCoordinatorDecision::kAlreadyRun,
		              "spawn-failure one-shot second run is rejected") &&
		       expect(callback_counts(context) == CallbackCounts{.spawn = 1},
		              "spawn-failure one-shot invokes spawn only once");
	}

	auto test_invalid_dependencies() -> bool {
		bool ok = true;
		for (int missing = 0; missing < 6; ++missing) {
			FakeContext context;
			auto        deps = dependencies(&context);
			switch (missing) {
				case 0:
					deps.spawn_compare_process = nullptr;
					break;
				case 1:
					deps.wait_for_compare_process = nullptr;
					break;
				case 2:
					deps.input_prompt_preflight = nullptr;
					break;
				case 3:
					deps.create_enter_device = nullptr;
					break;
				case 4:
					deps.create_native_prompt = nullptr;
					break;
				case 5:
					deps.request_auth_token = nullptr;
					break;
				default:
					break;
			}

			PromptCoordinator coordinator(nullptr, Workaround::kInput, true, false, deps,
			                              std::chrono::seconds(5));
			ok &= expect(!coordinator.valid(), "missing dependency is invalid");
			const auto before = callback_counts(context);
			const auto result = coordinator.run(make_compare_request());
			ok &= expect(result.decision == PromptCoordinatorDecision::kInvalidDependencies,
			             "invalid coordinator returns invalid-dependencies result");
			ok &= expect(callback_counts(context) == before,
			             "invalid coordinator invokes no callback");
		}
		return ok;
	}

	auto test_one_shot() -> bool {
		FakeContext context;
		const pid_t child_pid = spawn_child(EXIT_SUCCESS);
		if (!expect(child_pid > 0, "one-shot child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;

		PromptCoordinator coordinator(nullptr, Workaround::kOff, false, false,
		                              dependencies(&context), std::chrono::seconds(5));
		const auto        first = coordinator.run(make_compare_request());
		if (!expect(first.decision == PromptCoordinatorDecision::kHowdyResult,
		            "one-shot first run succeeds")) {
			return false;
		}
		const auto before = callback_counts(context);
		const auto second = coordinator.run(make_compare_request("/different/config.ini"));
		return expect(second.decision == PromptCoordinatorDecision::kAlreadyRun,
		              "one-shot second run is rejected") &&
		       expect(context.spawn_calls == 1 && context.spawned_pid == child_pid,
		              "one-shot first run spawns child once") &&
		       expect(context.wait_calls == 1 && context.waited_pid == child_pid,
		              "one-shot first run waits for spawned child") &&
		       expect(callback_counts(context) == before,
		              "one-shot second run invokes no callback") &&
		       expect(child_reaped(child_pid), "one-shot first run reaps child");
	}

}  // namespace

auto run_prompt_adapter_tests() -> bool {
	bool ok = true;
	ok &= test_direct_runtime_launch_request();
	ok &= test_staged_runtime_launch_request();
	ok &= test_production_direct_runtime_environment();
	ok &= test_production_staged_runtime_environment();
	ok &= test_owned_launch_request_from_temporaries();
	ok &= test_production_file_actions_init_failure();
	ok &= test_production_closefrom_failure();
	ok &= test_production_spawn_failure();
	ok &= test_spawn_failure();
	ok &= test_invalid_spawn_pid();
	ok &= test_one_shot_after_spawn_failure();
	ok &= test_invalid_dependencies();
	ok &= test_one_shot();
	return ok;
}
