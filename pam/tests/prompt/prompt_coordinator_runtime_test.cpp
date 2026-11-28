#include "prompt/prompt_coordinator.hpp"
#include "prompt/prompt_coordinator_fake.hpp"
#include "prompt/workaround.hpp"
#include "runtime/child_process.hpp"
#include "runtime/compare_launch.hpp"
#include "runtime/compare_process.hpp"
#include "runtime/compare_process_test_support.hpp"
#include "support/process_test_support.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <string>

#include <fcntl.h>
#include <unistd.h>

#include <sys/eventfd.h>

namespace {
	struct EventfdWrapState {
		bool fail_eventfd       = false;
		bool fail_eventfd_write = false;
	};

	auto EventfdState() -> EventfdWrapState & {
		static EventfdWrapState state;
		return state;
	}
}  // namespace

extern "C" auto TestEventfd(unsigned int value, int flags) -> int __asm__("__wrap_eventfd");
extern "C" auto RealEventfd(unsigned int value, int flags) -> int __asm__("__real_eventfd");
extern "C" auto TestEventfdWrite(int fd, eventfd_t value) -> int __asm__("__wrap_eventfd_write");
extern "C" auto RealEventfdWrite(int fd, eventfd_t value) -> int __asm__("__real_eventfd_write");

auto TestEventfd(unsigned int value, int flags) -> int {
	if (EventfdState().fail_eventfd) {
		errno = EMFILE;
		return -1;
	}
	return RealEventfd(value, flags);
}

auto TestEventfdWrite(int fd, eventfd_t value) -> int {
	if (EventfdState().fail_eventfd_write) {
		errno = EIO;
		return -1;
	}
	return RealEventfdWrite(fd, value);
}

namespace {
	using std::chrono_literals::operator""ms;
	using std::chrono_literals::operator""s;
	using howdy::pam::PromptCoordinator;
	using howdy::pam::PromptCoordinatorDecision;
	using howdy::pam::Workaround;
	using howdy::test::Expect;
	using howdy::test::process::ChildReaped;
	using howdy::test::process::SpawnBlockedChild;
	using howdy::test::process::SpawnChild;
	using howdy::test::prompt_coordinator::FakeContext;
	using howdy::test::prompt_coordinator::MakeCompareRequest;
	using howdy::test::prompt_coordinator::Timeout;
	using howdy::test::prompt_coordinator::TimeoutWaitStatus;

	auto WatchdogWaitForCompare(void *context, pid_t child_pid, int child_pidfd, int verdict_fd,
	                            std::chrono::steady_clock::time_point      deadline,
	                            void                                      *cancellation_context,
	                            howdy::pam::CompareCancellationRequestedFn cancellation_requested,
	                            int                                        cancellation_fd) -> int {
		auto &fake = *static_cast<FakeContext *>(context);
		++fake.wait_calls;
		fake.waited_pid      = child_pid;
		fake.waited_pidfd    = child_pidfd;
		const auto remaining = std::max(deadline - std::chrono::steady_clock::now(),
		                                std::chrono::steady_clock::duration::zero());
		// The fake spawn usually hands no pidfd; the test forked the child, so it may open one.
		const auto wait_deadline = std::chrono::steady_clock::now() + remaining;
		const int  status        = child_pidfd >= 0
		                               ? howdy::pam::compare_process::WaitUntil(
		                                     child_pid, child_pidfd, verdict_fd, wait_deadline,
		                                     cancellation_context, cancellation_requested, cancellation_fd)
		                               : howdy::test::prompt_coordinator::WaitUntilForTestChild(
		                                     child_pid, wait_deadline, cancellation_context,
		                                     cancellation_requested, cancellation_fd);
		if (cancellation_requested != nullptr && cancellation_requested(cancellation_context)) {
			++fake.terminate_calls;
			fake.terminated_pid = child_pid;
		}
		fake.last_wait_status = status;
		return status;
	}

	auto TestWatchdogTimeoutKeepsPasswordFallback() -> bool {
		FakeContext context{
		    .token_delay  = 100ms,
		    .token_result = PAM_SUCCESS,
		};
		const pid_t child_pid = SpawnBlockedChild();
		if (!Expect(child_pid > 0, "watchdog fallback child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;
		const auto ops         = Operations(&context, WatchdogWaitForCompare);

		PromptCoordinator coordinator(nullptr, Workaround::kInput, true, false, ops, Timeout(40ms));
		const auto        result = coordinator.Run(MakeCompareRequest());
		return Expect(result.decision == PromptCoordinatorDecision::kPasswordFallback,
		              "watchdog timeout keeps password fallback") &&
		       Expect(result.compare_status == TimeoutWaitStatus(),
		              "password fallback preserves watchdog timeout status") &&
		       Expect(result.pam_status == PAM_SUCCESS,
		              "password fallback preserves PAM success") &&
		       Expect(ChildReaped(child_pid), "password fallback reaps watchdog child");
	}

	auto TestPamSuccessReapsBeforeWatchdog() -> bool {
		FakeContext context;
		const pid_t child_pid = SpawnBlockedChild();
		if (!Expect(child_pid > 0, "PAM-before-watchdog child spawned")) {
			return false;
		}
		context.next_child_pid = child_pid;
		const auto ops         = Operations(&context, WatchdogWaitForCompare);

		PromptCoordinator coordinator(nullptr, Workaround::kInput, true, false, ops, Timeout(1s));
		const auto        result = coordinator.Run(MakeCompareRequest());
		return Expect(result.decision == PromptCoordinatorDecision::kPamResult,
		              "PAM success wins before watchdog") &&
		       Expect(context.terminate_calls == 1, "PAM success terminates compare child") &&
		       Expect(context.last_wait_status != TimeoutWaitStatus(),
		              "PAM success does not use watchdog timeout status") &&
		       Expect(ChildReaped(child_pid), "PAM success reaps compare child");
	}

	auto TestEventfdSetupFailure() -> bool {
		FakeContext       context;
		PromptCoordinator coordinator(nullptr, Workaround::kOff, false, false,
		                              Operations(&context, WatchdogWaitForCompare), Timeout(1s));
		EventfdState().fail_eventfd = true;
		const auto result           = coordinator.Run(MakeCompareRequest());
		EventfdState().fail_eventfd = false;
		return Expect(result.decision == PromptCoordinatorDecision::kPamResult &&
		                  result.pam_status == PAM_SYSTEM_ERR,
		              "eventfd failure fails closed") &&
		       Expect(context.spawn_calls == 0, "eventfd failure never spawns compare");
	}

	auto TestEventfdWriteFailure() -> bool {
		FakeContext context;
		const pid_t child = SpawnBlockedChild();
		if (!Expect(child > 0, "eventfd write failure spawns child")) {
			return false;
		}
		context.next_child_pid = child;
		PromptCoordinator coordinator(nullptr, Workaround::kInput, true, false,
		                              Operations(&context, WatchdogWaitForCompare), Timeout(40ms));
		EventfdState().fail_eventfd_write = true;
		const auto result                 = coordinator.Run(MakeCompareRequest());
		EventfdState().fail_eventfd_write = false;
		return Expect(result.decision == PromptCoordinatorDecision::kPamResult &&
		                  result.pam_status == PAM_SYSTEM_ERR,
		              "eventfd write failure overrides PAM success") &&
		       Expect(ChildReaped(child), "eventfd write failure leaves no child");
	}

	// The coordinator hands the spawn-time pidfd to the wait unchanged and closes it once the
	// run is over.
	auto TestCoordinatorCarriesSpawnPidfd() -> bool {
		const pid_t child_pid = SpawnChild(EXIT_SUCCESS);
		if (!Expect(child_pid > 0, "pidfd carry test spawns child")) {
			return false;
		}
		FakeContext context;
		context.next_child_pid          = child_pid;
		context.next_child_pidfd        = howdy::pam::runtime::OpenChildPidfd(child_pid);
		const int          handed_pidfd = context.next_child_pidfd;
		std::array<int, 2> verdict_socket{-1, -1};
		const bool         piped = pipe2(verdict_socket.data(), O_CLOEXEC) == 0;
		(void)close(verdict_socket.at(1));
		context.next_verdict_fd = verdict_socket.at(0);
		{
			PromptCoordinator coordinator(nullptr, Workaround::kOff, false, false,
			                              Operations(&context), Timeout());
			(void)coordinator.Run(MakeCompareRequest());
		}
		errno                     = 0;
		const bool closed         = fcntl(handed_pidfd, F_GETFD) < 0 && errno == EBADF;
		errno                     = 0;
		const bool verdict_closed = fcntl(verdict_socket.at(0), F_GETFD) < 0 && errno == EBADF;
		return Expect(handed_pidfd > STDERR_FILENO, "test opened a pidfd for the child") &&
		       Expect(context.waited_pidfd == handed_pidfd,
		              "wait borrows the pidfd taken at spawn") &&
		       Expect(closed, "coordinator closes the spawn pidfd after the run") &&
		       Expect(piped && context.waited_verdict_fd == verdict_socket.at(0),
		              "wait borrows the verdict socket taken at spawn") &&
		       Expect(verdict_closed, "coordinator closes the verdict socket after the run") &&
		       Expect(ChildReaped(child_pid), "pidfd carry test reaps the child");
	}

	auto TestWorkaroundSetupThrowCleansUpSpawnedChild() -> bool {
		struct ThrowCase {
			const char *label;
			bool        non_standard;
		};

		const auto cases = std::to_array<ThrowCase>({
		    {.label = "std::exception", .non_standard = false},
		    {.label = "non-standard exception", .non_standard = true},
		});

		bool ok = true;
		for (const auto &test_case : cases) {
			const pid_t child_pid = SpawnBlockedChild();
			if (!Expect(child_pid > 0, std::string(test_case.label) + ": spawns blocked child")) {
				return false;
			}
			FakeContext context;
			context.next_child_pid          = child_pid;
			context.next_child_pidfd        = howdy::pam::runtime::OpenChildPidfd(child_pid);
			const int          handed_pidfd = context.next_child_pidfd;
			std::array<int, 2> verdict_pipe{-1, -1};
			const bool         piped = pipe2(verdict_pipe.data(), O_CLOEXEC) == 0;
			(void)close(verdict_pipe.at(1));
			context.next_verdict_fd         = verdict_pipe.at(0);
			context.throw_preflight         = !test_case.non_standard;
			context.throw_preflight_unknown = test_case.non_standard;

			PromptCoordinator coordinator(nullptr, Workaround::kInput, true, false,
			                              Operations(&context, WatchdogWaitForCompare),
			                              Timeout(1s));
			const auto        result = coordinator.Run(MakeCompareRequest());

			errno                     = 0;
			const bool pidfd_closed   = fcntl(handed_pidfd, F_GETFD) < 0 && errno == EBADF;
			errno                     = 0;
			const bool verdict_closed = fcntl(verdict_pipe.at(0), F_GETFD) < 0 && errno == EBADF;
			ok &= Expect(handed_pidfd > STDERR_FILENO && piped,
			             std::string(test_case.label) + ": prepared pidfd and verdict fd");
			ok &= Expect(result.decision == PromptCoordinatorDecision::kPamResult &&
			                 result.pam_status == PAM_SYSTEM_ERR,
			             std::string(test_case.label) + ": maps setup throw to PAM_SYSTEM_ERR");
			ok &= Expect(context.terminate_calls == 1 && context.terminated_pid == child_pid,
			             std::string(test_case.label) + ": CleanupSpawnedChild cancels child");
			ok &= Expect(ChildReaped(child_pid),
			             std::string(test_case.label) + ": CleanupSpawnedChild reaps child");
			ok &= Expect(pidfd_closed && verdict_closed,
			             std::string(test_case.label) +
			                 ": CleanupSpawnedChild closes pidfd and verdict fd");
			ok &= Expect(context.auth_token_calls == 0,
			             std::string(test_case.label) + ": setup throw never requests auth token");
		}
		return ok;
	}

}  // namespace

auto main() -> int {
	bool ok = true;
	ok &= TestEventfdSetupFailure();
	ok &= TestEventfdWriteFailure();
	ok &= TestWatchdogTimeoutKeepsPasswordFallback();
	ok &= TestPamSuccessReapsBeforeWatchdog();
	ok &= TestCoordinatorCarriesSpawnPidfd();
	ok &= TestWorkaroundSetupThrowCleansUpSpawnedChild();
	return ok ? 0 : 1;
}
