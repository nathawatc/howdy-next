#ifndef _GNU_SOURCE
#	define _GNU_SOURCE
#endif

#include "runtime/compare_process.hpp"

#include "protocol/compare_exit.hpp"
#include "runtime/child_process.hpp"
#include "runtime/compare_launch.hpp"
#include "support/bit_flags.hpp"
#include "support/eintr.hpp"
#include "support/error_message.hpp"
#include "support/process_status.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <optional>

#include <signal.h>
#include <time.h>
#include <unistd.h>

#include <sys/poll.h>
#include <sys/syslog.h>
#include <sys/types.h>

namespace {
	// Lets compare process perform SIGTERM cleanup without extending scan deadline.
	constexpr auto kCompareTerminationGrace = std::chrono::milliseconds(250);

	using howdy::native::CompareExit;

	using howdy::pam::runtime::ChildState;

	// The compare child with this module's logging and status policy; the pidfd and PID-fallback
	// rules live in runtime::TrackedChild. `verdict_fd` is the borrowed PAM end of the verdict
	// socket (protocol/compare_verdict.hpp), or -1. Once any wait or signal reports the child
	// reaped elsewhere (ECHILD), Gone() is true and the child is never touched again.
	class CompareChild {
	public:
		// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
		CompareChild(pid_t pid, int pidfd, int verdict_fd)
		    : child_(pid, pidfd)
		    , verdict_fd_(verdict_fd) {}

		// Non-blocking. On kError `status` holds the abort status, and a child that may still be
		// running (any failure but ECHILD) has been stopped, as the helper path does.
		auto TryWait(int *status) -> ChildState {
			const auto state = child_.Wait(false, status);
			if (state == ChildState::kError) {
				const int error_number = errno;
				*status                = howdy::native::EncodeWaitExitStatus(CompareExit::kAbort);
				if (error_number == ECHILD) {
					return state;
				}
				syslog(LOG_AUTHPRIV | LOG_ERR,
				       "pam_howdy: wait failed for compare process: %s (%d)",
				       howdy::native::SystemErrorMessage(error_number).c_str(), error_number);
				(void)howdy::pam::runtime::KillAndReap(child_);
			}
			return state;
		}

		// TryWait for the check whose status is the result: a compare the application reaped
		// reports through its verdict instead of the exit status it took.
		auto TryWaitForResult(int *status) -> ChildState {
			const auto state = TryWait(status);
			if (Gone()) {
				*status = ReapedElsewhereStatus();
			}
			return state;
		}

		// Blocking reap. nullopt when the child could not be reaped, including when the
		// application reaped it (Gone()).
		auto Reap() -> std::optional<int> {
			int status = 0;
			if (child_.Wait(true, &status) == ChildState::kReaped) {
				return status;
			}
			const int error_number = errno;
			if (error_number == ECHILD) {
				return std::nullopt;
			}
			syslog(LOG_AUTHPRIV | LOG_ERR,
			       "pam_howdy: wait failed while reaping compare process: %s (%d)",
			       howdy::native::SystemErrorMessage(error_number).c_str(), error_number);
			return std::nullopt;
		}

		// Reap for a child that exited on its own, whose status is the result.
		auto ReapExited() -> int {
			if (const auto status = Reap()) {
				return *status;
			}
			return Gone() ? ReapedElsewhereStatus()
			              : howdy::native::EncodeWaitExitStatus(CompareExit::kAbort);
		}

		// False means the child could not be signalled at all; do not block on it afterwards.
		[[nodiscard]] auto Signal(int signal) -> bool {
			const int error_number = child_.Signal(signal);
			if (error_number == 0) {
				return true;
			}
			if (error_number == ECHILD) {
				return false;
			}
			syslog(LOG_AUTHPRIV | LOG_ERR,
			       "pam_howdy: Failed to signal compare process (%d): %s (%d)", signal,
			       howdy::native::SystemErrorMessage(error_number).c_str(), error_number);
			return false;
		}

		// The child exited and the application reaped it: there is nothing left to stop or reap.
		[[nodiscard]] auto Gone() const -> bool {
			return child_.WasReapedElsewhere();
		}

	private:
		// Only called once the application has reaped compare, so compare has exited and its
		// write, if any, is in the socket. The verdict counts only as the one byte the socket
		// holds; anything else, or no byte at all, fails closed.
		[[nodiscard]] auto ReapedElsewhereStatus() const -> int {
			if (verdict_fd_ < 0) {
				// Without a verdict socket there is nothing to read; the abort that follows is
				// reported where the result is mapped.
				syslog(LOG_AUTHPRIV | LOG_NOTICE,
				       "pam_howdy: compare process was reaped elsewhere");
				return howdy::native::EncodeWaitExitStatus(CompareExit::kAbort);
			}
			// Two bytes of room, so a longer record shows up as more than one byte instead of
			// being cut to its first.
			std::array<unsigned char, 2> record{};
			const auto                   read_record = [this, &record] -> ssize_t {
				return howdy::native::RetryOnEintr([&] -> ssize_t {
					return read(verdict_fd_, record.data(), record.size());
				});
			};
			const ssize_t first = read_record();
			if (first != 1) {
				const int error_number = errno;
				if (first == 0 ||
				    (first < 0 && (error_number == EAGAIN || error_number == EWOULDBLOCK))) {
					syslog(LOG_AUTHPRIV | LOG_ERR,
					       "pam_howdy: compare process was reaped elsewhere without reporting a "
					       "result");
				} else if (first > 1) {
					syslog(LOG_AUTHPRIV | LOG_ERR,
					       "pam_howdy: compare process was reaped elsewhere and reported a "
					       "malformed result");
				} else {
					syslog(LOG_AUTHPRIV | LOG_ERR,
					       "pam_howdy: compare result could not be read: %s (%d)",
					       howdy::native::SystemErrorMessage(error_number).c_str(), error_number);
				}
				return howdy::native::EncodeWaitExitStatus(CompareExit::kAbort);
			}
			const unsigned char verdict = record.front();
			const ssize_t       more    = read_record();
			const bool          drained =
			    more == 0 || (more < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
			if (!drained) {
				syslog(LOG_AUTHPRIV | LOG_ERR,
				       "pam_howdy: compare process was reaped elsewhere and reported more than one "
				       "result");
				return howdy::native::EncodeWaitExitStatus(CompareExit::kAbort);
			}
			// Normal on such hosts, so not worth a line on every authentication.
			syslog(LOG_AUTHPRIV | LOG_DEBUG,
			       "pam_howdy: compare process was reaped elsewhere; using its reported result %u",
			       static_cast<unsigned int>(verdict));
			return howdy::native::EncodeWaitExitStatus(verdict);
		}

		howdy::pam::runtime::TrackedChild child_;
		int                               verdict_fd_;
	};

	enum class WaitOutcome : std::uint8_t {
		kExited,
		kCancelled,
		kTimeout,
		kError,
	};

	auto PollCompareUntil(int pidfd, int cancellation_fd,
	                      std::chrono::steady_clock::time_point deadline) -> WaitOutcome {
		using Clock = std::chrono::steady_clock;
		while (true) {
			const auto     remaining = std::max(deadline - Clock::now(), Clock::duration::zero());
			const auto     seconds   = std::chrono::duration_cast<std::chrono::seconds>(remaining);
			const timespec timeout{
			    .tv_sec  = seconds.count(),
			    .tv_nsec = std::chrono::duration_cast<std::chrono::nanoseconds>(remaining - seconds)
				               .count(),
			};
			std::array<pollfd, 2> descriptors{
			    {
			        {.fd = pidfd, .events = POLLIN, .revents = 0},
			        {.fd = cancellation_fd, .events = POLLIN, .revents = 0},
			    },
			};
			const int result = ppoll(descriptors.data(), descriptors.size(), &timeout, nullptr);
			if (result < 0) {
				if (errno == EINTR) {
					continue;
				}
				const int error_number = errno;
				syslog(LOG_AUTHPRIV | LOG_ERR,
				       "pam_howdy: ppoll failed for compare process: %s (%d)",
				       howdy::native::SystemErrorMessage(error_number).c_str(), error_number);
				return WaitOutcome::kError;
			}
			const auto pidfd_events        = descriptors.front().revents;
			const auto cancellation_events = descriptors.back().revents;
			// POLLHUP on a pidfd means the child has been reaped (Linux 6.9+), which the reap that
			// follows reports; on the cancellation eventfd it is an error.
			if (howdy::native::HasAnyBits(pidfd_events, POLLERR, POLLNVAL) ||
			    howdy::native::HasAnyBits(cancellation_events, POLLERR, POLLHUP, POLLNVAL)) {
				syslog(LOG_AUTHPRIV | LOG_ERR,
				       "pam_howdy: Unexpected compare poll events: pidfd %#x, cancellation %#x",
				       static_cast<unsigned int>(pidfd_events),
				       static_cast<unsigned int>(cancellation_events));
				return WaitOutcome::kError;
			}
			if (howdy::native::HasAnyBits(pidfd_events, POLLIN, POLLHUP)) {
				return WaitOutcome::kExited;
			}
			if (howdy::native::HasAnyBits(cancellation_events, POLLIN)) {
				return WaitOutcome::kCancelled;
			}
			if (Clock::now() >= deadline) {
				return WaitOutcome::kTimeout;
			}
		}
	}

	// Ends a compare that had not exited when the wait reached its outcome (timeout,
	// cancellation, or a poll error); the decision is made.
	auto EndUnfinishedCompare(CompareChild &child, int child_pidfd, WaitOutcome outcome) -> int {
		using Clock            = std::chrono::steady_clock;
		const int abort_status = howdy::native::EncodeWaitExitStatus(CompareExit::kAbort);
		int       terminal_status =
		    outcome == WaitOutcome::kTimeout
		        ? howdy::native::EncodeWaitExitStatus(CompareExit::kTimeoutReached)
		        : abort_status;
		// A child that is gone, reaped by this wait or by the application, ends with the decision;
		// one that may still be running ends with an abort.
		const auto end = [&](bool reaped) -> int {
			if (child.Gone()) {
				syslog(LOG_AUTHPRIV | LOG_NOTICE,
				       "pam_howdy: compare process was reaped elsewhere after the wait ended");
				return terminal_status;
			}
			return reaped ? terminal_status : abort_status;
		};
		int        status      = 0;
		const auto child_state = child.TryWait(&status);
		if (child_state != ChildState::kRunning) {
			return end(child_state == ChildState::kReaped);
		}
		if (outcome != WaitOutcome::kError) {
			if (!child.Signal(SIGTERM)) {
				if (child.Gone()) {
					return end(false);
				}
				terminal_status = abort_status;
			}
			// Also after a refused signal: compare may still end on its own within the grace, and
			// one that is never reaped stays a zombie in the PAM application.
			const auto grace =
			    PollCompareUntil(child_pidfd, -1, Clock::now() + kCompareTerminationGrace);
			if (grace == WaitOutcome::kExited) {
				return end(child.Reap().has_value());
			}
			if (grace == WaitOutcome::kError) {
				terminal_status = abort_status;
			}
		}
		if (!child.Signal(SIGKILL)) {
			if (child.Gone()) {
				return end(false);
			}
			// A blocking reap would wait forever on a child that was never signalled; still
			// collect it if it has exited meanwhile.
			(void)child.TryWait(&status);
			return abort_status;
		}
		return end(child.Reap().has_value());
	}

	void KillAndReapCompare(pid_t child_pid, int child_pidfd) noexcept {
		howdy::pam::runtime::TrackedChild child(child_pid, child_pidfd);
		(void)howdy::pam::runtime::KillAndReap(child);
	}

	// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
	auto WaitForCompareProcess(pid_t child_pid, int child_pidfd, int verdict_fd,
	                           std::chrono::steady_clock::time_point      deadline,
	                           void                                      *cancellation_context,
	                           howdy::pam::CompareCancellationRequestedFn cancellation_requested,
	                           int                                        cancellation_fd) -> int {
		using Clock            = std::chrono::steady_clock;
		const int abort_status = howdy::native::EncodeWaitExitStatus(CompareExit::kAbort);
		if (child_pid <= 0) {
			syslog(LOG_AUTHPRIV | LOG_ERR, "pam_howdy: Invalid compare child pid: %d", child_pid);
			return abort_status;
		}
		// The pidfd taken at spawn is borrowed and required: it is what the wait polls, and one
		// opened now could name a process that reused the PID. Without it, stop the child by PID
		// and fail closed.
		if (child_pidfd < 0) {
			syslog(LOG_AUTHPRIV | LOG_ERR, "pam_howdy: No pidfd for compare process %d", child_pid);
			KillAndReapCompare(child_pid, -1);
			return abort_status;
		}

		// Preserve exit-before-cancellation precedence and the terminal decision once made.
		CompareChild child(child_pid, child_pidfd, verdict_fd);
		int          status = 0;
		if (child.TryWaitForResult(&status) != ChildState::kRunning) {
			return status;
		}
		WaitOutcome outcome = WaitOutcome::kTimeout;
		if (cancellation_requested != nullptr && cancellation_requested(cancellation_context)) {
			outcome = WaitOutcome::kCancelled;
		} else if (Clock::now() < deadline) {
			outcome = PollCompareUntil(child_pidfd, cancellation_fd, deadline);
		}
		if (outcome == WaitOutcome::kExited) {
			return child.ReapExited();
		}
		return EndUnfinishedCompare(child, child_pidfd, outcome);
	}

}  // namespace

namespace howdy::pam::compare_process {

	auto WaitUntil(pid_t child_pid, int child_pidfd, int verdict_fd,
	               std::chrono::steady_clock::time_point deadline, void *cancellation_context,
	               CompareCancellationRequestedFn cancellation_requested, int cancellation_fd)
	    -> int {
		return WaitForCompareProcess(child_pid, child_pidfd, verdict_fd, deadline,
		                             cancellation_context, cancellation_requested, cancellation_fd);
	}

	auto Wait([[maybe_unused]] void *context, pid_t child_pid, int child_pidfd, int verdict_fd,
	          std::chrono::steady_clock::time_point deadline, void *cancellation_context,
	          CompareCancellationRequestedFn cancellation_requested, int cancellation_fd) -> int {
		return WaitUntil(child_pid, child_pidfd, verdict_fd, deadline, cancellation_context,
		                 cancellation_requested, cancellation_fd);
	}

	void CancelAndReap(pid_t child_pid, int child_pidfd) noexcept {
		try {
			(void)WaitForCompareProcess(child_pid, child_pidfd, -1,
			                            std::chrono::steady_clock::now(), nullptr, nullptr, -1);
		} catch (...) {
			KillAndReapCompare(child_pid, child_pidfd);
		}
	}

}  // namespace howdy::pam::compare_process
