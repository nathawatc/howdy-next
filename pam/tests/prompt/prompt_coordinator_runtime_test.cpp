#include "paths.hpp"
#include "prompt/prompt_coordinator_fake.hpp"
#include "runtime/compare_process.hpp"
#include "runtime/compare_process_test_support.hpp"
#include "support/process_test_support.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <string>
#include <vector>

namespace {
	using namespace howdy::test::process;
	using namespace howdy::test::prompt_coordinator;

	using howdy::test::expect;

	auto WatchdogWaitForCompare(void *context, pid_t child_pid,
	                            std::chrono::steady_clock::time_point      deadline,
	                            void                                      *cancellation_context,
	                            howdy::pam::CompareCancellationRequestedFn cancellation_requested)
	    -> int {
		auto &fake = *static_cast<FakeContext *>(context);
		++fake.wait_calls;
		fake.waited_pid      = child_pid;
		const auto remaining = std::max(deadline - std::chrono::steady_clock::now(),
		                                std::chrono::steady_clock::duration::zero());
		const int  status    = howdy::pam::compare_process::WaitUntil(
		    child_pid, std::chrono::steady_clock::now() + remaining, cancellation_context,
		    cancellation_requested);
		if (cancellation_requested != nullptr && cancellation_requested(cancellation_context)) {
			++fake.terminate_calls;
			fake.terminated_pid = child_pid;
		}
		fake.last_wait_status = status;
		return status;
	}

	auto TestWatchdogTimeoutReapsBlockedChild() -> bool {
		const pid_t child_pid = SpawnBlockedChild();
		if (!expect(child_pid > 0, "watchdog timeout child spawned")) {
			return false;
		}

		const int status = howdy::pam::compare_process::WaitUntil(
		    child_pid, std::chrono::steady_clock::now() + 40ms);
		return expect(status == TimeoutWaitStatus(),
		              "watchdog timeout returns synthetic timeout status") &&
		       expect(ChildReaped(child_pid), "watchdog timeout reaps blocked child");
	}

	auto TestWatchdogKillsSigtermIgnoringChild() -> bool {
		const pid_t child_pid = SpawnSigtermIgnoringChild();
		if (!expect(child_pid > 0, "SIGTERM-ignoring watchdog child spawned")) {
			return false;
		}

		const int status = howdy::pam::compare_process::WaitUntil(
		    child_pid, std::chrono::steady_clock::now() + 40ms);
		return expect(status == TimeoutWaitStatus(),
		              "SIGTERM-ignoring child returns synthetic timeout status") &&
		       expect(ChildReaped(child_pid), "SIGTERM-ignoring child is SIGKILLed and reaped");
	}

	auto TestWatchdogPreservesNaturalExitStatus() -> bool {
		const pid_t child_pid = SpawnChild(17, 10ms);
		if (!expect(child_pid > 0, "natural watchdog child spawned")) {
			return false;
		}

		const int status = howdy::pam::compare_process::WaitUntil(
		    child_pid, std::chrono::steady_clock::now() + 1s);
		return expect(status == (17 << 8), "watchdog preserves natural exit wait status") &&
		       expect(ChildReaped(child_pid), "watchdog reaps naturally exited child");
	}

	auto TestWatchdogBlockedSigchldNaturalExit() -> bool {
		ScopedSignalBlock blocked_sigchld(SIGCHLD);
		if (!expect(blocked_sigchld.Valid(), "blocked SIGCHLD guard installs")) {
			return false;
		}

		const pid_t child_pid = SpawnChild(17, 10ms);
		if (!expect(child_pid > 0, "blocked SIGCHLD natural-exit child spawned")) {
			return false;
		}

		const int status = howdy::pam::compare_process::WaitUntil(
		    child_pid, std::chrono::steady_clock::now() + 1s);
		return expect(status == (17 << 8),
		              "blocked SIGCHLD preserves natural compare exit status") &&
		       expect(ChildReaped(child_pid), "blocked SIGCHLD reaps natural compare child");
	}

	auto TestWatchdogBlockedSigchldTimeout() -> bool {
		ScopedSignalBlock blocked_sigchld(SIGCHLD);
		if (!expect(blocked_sigchld.Valid(), "blocked SIGCHLD timeout guard installs")) {
			return false;
		}

		const pid_t child_pid = SpawnBlockedChild();
		if (!expect(child_pid > 0, "blocked SIGCHLD timeout child spawned")) {
			return false;
		}

		const auto start   = std::chrono::steady_clock::now();
		const int  status  = howdy::pam::compare_process::WaitUntil(child_pid, start + 40ms);
		const auto elapsed = std::chrono::steady_clock::now() - start;
		return expect(status == TimeoutWaitStatus(),
		              "blocked SIGCHLD returns synthetic compare timeout status") &&
		       expect(elapsed < 2s, "blocked SIGCHLD compare timeout remains bounded") &&
		       expect(ChildReaped(child_pid), "blocked SIGCHLD reaps timed-out compare child");
	}

	auto TestWatchdogBlockedSigtermTimeout() -> bool {
		ScopedSignalBlock blocked_sigterm(SIGTERM);
		if (!expect(blocked_sigterm.Valid(), "blocked SIGTERM timeout guard installs")) {
			return false;
		}

		const pid_t child_pid = SpawnBlockedChild();
		if (!expect(child_pid > 0, "blocked SIGTERM timeout child spawned")) {
			return false;
		}

		const auto start   = std::chrono::steady_clock::now();
		const int  status  = howdy::pam::compare_process::WaitUntil(child_pid, start + 40ms);
		const auto elapsed = std::chrono::steady_clock::now() - start;
		return expect(status == TimeoutWaitStatus(),
		              "blocked SIGTERM returns synthetic compare timeout status") &&
		       expect(elapsed >= 40ms, "blocked SIGTERM compare timeout honors deadline") &&
		       expect(elapsed < 2s, "blocked SIGTERM compare timeout remains bounded") &&
		       expect(ChildReaped(child_pid),
		              "blocked SIGTERM falls back to SIGKILL and reaps compare child");
	}

	auto TestWatchdogTimeoutKeepsPasswordFallback() -> bool {
		FakeContext context{
		    .token_delay  = 100ms,
		    .token_result = PAM_SUCCESS,
		};
		const pid_t child_pid = SpawnBlockedChild();
		if (!expect(child_pid > 0, "watchdog fallback child spawned")) {
			return false;
		}
		context.next_child_pid        = child_pid;
		auto deps                     = Dependencies(&context);
		deps.wait_for_compare_process = WatchdogWaitForCompare;

		PromptCoordinator coordinator(nullptr, Workaround::kInput, true, false, deps, 40ms);
		const auto        result = coordinator.Run(MakeCompareRequest());
		return expect(result.decision == PromptCoordinatorDecision::kPasswordFallback,
		              "watchdog timeout keeps password fallback") &&
		       expect(result.compare_status == TimeoutWaitStatus(),
		              "password fallback preserves watchdog timeout status") &&
		       expect(result.pam_status == PAM_SUCCESS,
		              "password fallback preserves PAM success") &&
		       expect(ChildReaped(child_pid), "password fallback reaps watchdog child");
	}

	auto TestPamSuccessReapsBeforeWatchdog() -> bool {
		FakeContext context;
		const pid_t child_pid = SpawnBlockedChild();
		if (!expect(child_pid > 0, "PAM-before-watchdog child spawned")) {
			return false;
		}
		context.next_child_pid        = child_pid;
		auto deps                     = Dependencies(&context);
		deps.wait_for_compare_process = WatchdogWaitForCompare;

		PromptCoordinator coordinator(nullptr, Workaround::kInput, true, false, deps, 1s);
		const auto        result = coordinator.Run(MakeCompareRequest());
		return expect(result.decision == PromptCoordinatorDecision::kPamResult,
		              "PAM success wins before watchdog") &&
		       expect(context.terminate_calls == 1, "PAM success terminates compare child") &&
		       expect(context.last_wait_status != TimeoutWaitStatus(),
		              "PAM success does not use watchdog timeout status") &&
		       expect(ChildReaped(child_pid), "PAM success reaps compare child");
	}

	auto TestProductionSpawnAdapter(const howdy::pam::CompareLaunchRequest &request,
	                                const std::vector<std::string>         &expected_argv,
	                                const std::string                      &label,
	                                const std::vector<std::string> &expected_environment) -> bool {
		PosixSpawnCapture capture;
		pid_t             child_pid = -1;
		const int         result =
		    howdy::pam::compare_process::Spawn(request, &child_pid, PosixSpawnOperations(&capture));

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

	auto TestProductionDirectRuntimeEnvironment() -> bool {
		return TestProductionSpawnAdapter(
		    MakeCompareRequest("/etc/howdy/config.ini", "alice", "/etc/howdy/models", false),
		    {kCompareProcessPath, "--config", "/etc/howdy/config.ini", "alice"},
		    "production direct runtime", {});
	}

	auto TestProductionStagedRuntimeEnvironment() -> bool {
		return TestProductionSpawnAdapter(
		    MakeCompareRequest("/run/howdy/runtime/config.ini", "alice",
		                       "/run/howdy/runtime/models", true),
		    {kCompareProcessPath, "--config", "/run/howdy/runtime/config.ini", "alice"},
		    "production staged runtime", {"HOWDY_USER_MODELS_DIR=/run/howdy/runtime/models"});
	}

	auto TestOwnedLaunchRequestFromTemporaries() -> bool {
		const howdy::pam::CompareLaunchRequest request = {
		    .config_path     = std::string("/run/howdy/temporary/config.ini"),
		    .username        = std::string("temporary-user"),
		    .user_models_dir = std::string("/run/howdy/temporary/models"),
		    .staged_runtime  = true,
		};

		return TestProductionSpawnAdapter(
		    request,
		    {kCompareProcessPath, "--config", "/run/howdy/temporary/config.ini", "temporary-user"},
		    "owned temporary request", {"HOWDY_USER_MODELS_DIR=/run/howdy/temporary/models"});
	}

	auto TestProductionFileActionsInitFailure() -> bool {
		PosixSpawnCapture capture;
		capture.init_result = ENOMEM;
		pid_t     child_pid = -1;
		const int result    = howdy::pam::compare_process::Spawn(MakeCompareRequest(), &child_pid,
		                                                         PosixSpawnOperations(&capture));

		return expect(result == ENOMEM, "file-actions init failure preserves error") &&
		       expect(capture.init_calls == 1, "file-actions init failure initializes once") &&
		       expect(capture.addclosefrom_calls == 0,
		              "file-actions init failure does not add close-from action") &&
		       expect(capture.spawn_calls == 0, "file-actions init failure does not spawn") &&
		       expect(capture.destroy_calls == 0,
		              "file-actions init failure does not destroy uninitialized actions") &&
		       expect(child_pid == -1, "file-actions init failure leaves child PID unchanged");
	}

	auto TestProductionClosefromFailure() -> bool {
		PosixSpawnCapture capture;
		capture.addclosefrom_result = EINVAL;
		pid_t     child_pid         = -1;
		const int result = howdy::pam::compare_process::Spawn(MakeCompareRequest(), &child_pid,
		                                                      PosixSpawnOperations(&capture));

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
		              "close-from setup failure destroys initialized file actions") &&
		       expect(child_pid == -1, "close-from setup failure leaves child PID unchanged");
	}

	auto TestProductionSpawnFailure() -> bool {
		PosixSpawnCapture capture;
		capture.spawn_result = EACCES;
		pid_t     child_pid  = -1;
		const int result     = howdy::pam::compare_process::Spawn(MakeCompareRequest(), &child_pid,
		                                                          PosixSpawnOperations(&capture));

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

}  // namespace

auto main() -> int {
	bool ok = true;
	ok &= TestWatchdogTimeoutReapsBlockedChild();
	ok &= TestWatchdogKillsSigtermIgnoringChild();
	ok &= TestWatchdogPreservesNaturalExitStatus();
	ok &= TestWatchdogBlockedSigchldNaturalExit();
	ok &= TestWatchdogBlockedSigchldTimeout();
	ok &= TestWatchdogBlockedSigtermTimeout();
	ok &= TestWatchdogTimeoutKeepsPasswordFallback();
	ok &= TestPamSuccessReapsBeforeWatchdog();
	ok &= TestProductionDirectRuntimeEnvironment();
	ok &= TestProductionStagedRuntimeEnvironment();
	ok &= TestOwnedLaunchRequestFromTemporaries();
	ok &= TestProductionFileActionsInitFailure();
	ok &= TestProductionClosefromFailure();
	ok &= TestProductionSpawnFailure();
	return ok ? 0 : 1;
}
