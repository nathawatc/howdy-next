#include "protocol/compare_exit.hpp"
#include "runtime/auth_helper_process.hpp"
#include "runtime/compare_process.hpp"
#include "support/process_test_support.hpp"
#include "test_support.hpp"

#include <chrono>
#include <limits>

auto run_process_wait_tests() -> bool;

namespace {
	using namespace howdy::test::process;
	using howdy::test::expect;

	auto always_cancel_compare(void *context) -> bool {
		(void)context;
		return true;
	}

	auto expect_process_waiting() -> bool {
		bool        ok                = true;
		const pid_t compare_child_pid = spawn_exiting_child(7);
		ok &= expect(compare_child_pid > 0, "spawns compare child");
		if (compare_child_pid > 0) {
			const int status = howdy::pam::compare_process::wait_until(
			    compare_child_pid, std::chrono::steady_clock::now() + std::chrono::seconds(1));
			ok &= expect(WIFEXITED(status) && WEXITSTATUS(status) == 7,
			             "compare wait preserves child exit status");
		}

		const pid_t helper_child_pid = spawn_exiting_child(9);
		ok &= expect(helper_child_pid > 0, "spawns helper child");
		if (helper_child_pid > 0) {
			const int status = howdy::pam::auth_helper_process::wait_for_helper(helper_child_pid);
			ok &= expect(WIFEXITED(status) && WEXITSTATUS(status) == 9,
			             "helper wait preserves child exit status");
		}

		constexpr pid_t kNonexistentChild = std::numeric_limits<pid_t>::max();
		const int       compare_failure   = howdy::pam::compare_process::wait_until(
		    kNonexistentChild, std::chrono::steady_clock::now() + std::chrono::seconds(1));
		const auto abort_code = static_cast<int>(howdy::native::CompareExit::kAbort);
		ok &= expect(WIFEXITED(compare_failure) && WEXITSTATUS(compare_failure) == abort_code,
		             "compare wait failure returns abort status");
		const int helper_failure =
		    howdy::pam::auth_helper_process::wait_for_helper(kNonexistentChild);
		ok &= expect(WIFEXITED(helper_failure) && WEXITSTATUS(helper_failure) == abort_code,
		             "helper wait failure returns abort status");

		const pid_t cancelled_child = spawn_blocked_child();
		ok &= expect(cancelled_child > 0, "spawns cancellable compare child");
		if (cancelled_child > 0) {
			const int cancelled_status = howdy::pam::compare_process::wait_until(
			    cancelled_child, std::chrono::steady_clock::now() + std::chrono::seconds(1),
			    nullptr, always_cancel_compare);
			ok &= expect(WIFEXITED(cancelled_status) && WEXITSTATUS(cancelled_status) == abort_code,
			             "compare cancellation returns abort status");
		}

		const pid_t timed_out_child = spawn_sigterm_ignoring_child();
		ok &= expect(timed_out_child > 0, "spawns SIGTERM-resistant compare child");
		if (timed_out_child > 0) {
			const int timed_out_status = howdy::pam::compare_process::wait_until(
			    timed_out_child, std::chrono::steady_clock::now());
			const int timeout_code = static_cast<int>(howdy::native::CompareExit::kTimeoutReached);
			ok &=
			    expect(WIFEXITED(timed_out_status) && WEXITSTATUS(timed_out_status) == timeout_code,
			           "compare timeout kills SIGTERM-resistant child");
		}

		return ok;
	}

}  // namespace

auto run_process_wait_tests() -> bool {
	return expect_process_waiting();
}
