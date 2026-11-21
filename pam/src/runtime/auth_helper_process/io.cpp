#ifndef _GNU_SOURCE
#	define _GNU_SOURCE
#endif

#include "internal.hpp"
#include "runtime/child_process.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>

#include <signal.h>
#include <stdlib.h>

#include <sys/poll.h>
#include <sys/syslog.h>
#include <sys/types.h>

namespace howdy::pam::auth_helper_process::internal {

	auto DeadlinePollTimeout(HelperDeadline deadline) -> int {
		const auto remaining = deadline - std::chrono::steady_clock::now();
		if (remaining <= HelperDeadline::duration::zero()) {
			return 0;
		}
		return static_cast<int>(std::chrono::ceil<std::chrono::milliseconds>(remaining).count());
	}

	namespace {

		using howdy::pam::runtime::ChildState;
		using howdy::pam::runtime::TrackedChild;

		constexpr auto kHelperTerminationGrace = std::chrono::milliseconds(500);
		constexpr auto kHelperExitRecheck      = std::chrono::milliseconds(10);

		void LogHelperSignalFailure(const char *action, int error_number) noexcept {
			syslog(LOG_AUTHPRIV | LOG_WARNING, "pam_howdy: Failed to %s auth helper process (%d)",
			       action, error_number);
		}

		// Signal confirms the helper is still ours before sending anything.
		void TerminateAndReapTracked(TrackedChild &child) noexcept {
			if (const int error_number = child.Signal(SIGTERM); error_number != 0) {
				if (error_number == ECHILD) {
					return;  // reaped elsewhere: nothing of ours left to stop
				}
				LogHelperSignalFailure("terminate", error_number);
			} else {
				// The grace period only means something once SIGTERM was delivered.
				(void)child.AwaitExit(kHelperTerminationGrace);
			}
			// Reaps a helper that exited during the grace period, and kills one that did not.
			(void)howdy::pam::runtime::KillAndReap(child);
		}

		// The result for a helper reaped elsewhere: past the deadline it counts as a timeout, as a
		// helper the wait collects late does.
		auto ReapedElsewhereResult(HelperDeadline deadline) noexcept -> HelperWaitResult {
			return std::chrono::steady_clock::now() >= deadline
			           ? HelperWaitResult::kTimedOut
					   : HelperWaitResult::kReapedElsewhere;
		}

	}  // namespace

	// Every helper wait and signal goes through TrackedChild, so a helper the PAM application
	// reaped itself is reported as kReapedElsewhere and never signalled.
	void TerminateAndReapHelperProcess(pid_t child_pid, int pidfd) noexcept {
		// Signal confirms the helper is still ours before sending anything, and a helper that
		// already exited is reaped after the (immediate) grace wait.
		TrackedChild child(child_pid, pidfd);
		TerminateAndReapTracked(child);
	}

	auto WaitForHelperProcessUntil(pid_t child_pid, HelperDeadline deadline, int *status,
	                               int pidfd) noexcept -> HelperWaitResult {
		TrackedChild child(child_pid, pidfd);
		bool         exit_seen = false;
		while (true) {
			switch (child.Wait(false, status)) {
				case ChildState::kReaped:
					return std::chrono::steady_clock::now() >= deadline
					           ? HelperWaitResult::kTimedOut
							   : HelperWaitResult::kExited;
				case ChildState::kError:
					// ECHILD: reaped elsewhere, so there is nothing left that is ours to stop, and
					// the exit status is gone. Any other failure leaves a live helper; stop it.
					if (errno == ECHILD) {
						return ReapedElsewhereResult(deadline);
					}
					TerminateAndReapTracked(child);
					return HelperWaitResult::kWaitError;
				case ChildState::kRunning:
					break;
			}
			if (std::chrono::steady_clock::now() >= deadline) {
				TerminateAndReapTracked(child);
				return HelperWaitResult::kTimedOut;
			}
			// DeadlinePollTimeout never goes negative, which poll would treat as wait forever, and
			// is recomputed after the pause, so the wait never overshoots the deadline.
			const auto until_deadline = [deadline] -> std::chrono::milliseconds {
				return std::chrono::milliseconds(DeadlinePollTimeout(deadline));
			};
			// An exit the wait cannot collect yet (a tracer holds it) keeps the pidfd readable;
			// pause between checks instead of spinning on it.
			if (exit_seen) {
				(void)poll(
				    nullptr, 0,
				    static_cast<int>(std::min(until_deadline(), kHelperExitRecheck).count()));
			}
			// Wakes as soon as the helper exits, or at the deadline.
			exit_seen = child.AwaitExit(until_deadline());
		}
	}

}  // namespace howdy::pam::auth_helper_process::internal
