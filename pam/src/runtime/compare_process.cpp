#ifndef _GNU_SOURCE
#	define _GNU_SOURCE
#endif

#include "runtime/compare_process.hpp"

#include "paths.hpp"
#include "protocol/compare_exit.hpp"
#include "protocol/compare_invocation.hpp"
#include "protocol/compare_verdict.hpp"
#include "runtime/child_process.hpp"
#include "runtime/compare_launch.hpp"
#include "support/bit_flags.hpp"
#include "support/eintr.hpp"
#include "support/error_message.hpp"
#include "support/process_status.hpp"
#include "support/scoped_fd.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/syslog.h>
#include <sys/types.h>

namespace {
	using CompareOperations   = howdy::pam::compare_process::Operations;
	using CompareSpawnRequest = howdy::pam::compare_process::SpawnRequest;

	// Fixed compare environment entries that keep a threaded BLAS from reserving address space per
	// CPU thread. OpenBLAS built with OpenMP (Arch) obeys OMP_NUM_THREADS and ignores
	// OPENBLAS_NUM_THREADS; pthread builds obey OPENBLAS_NUM_THREADS.
	constexpr auto kCompareThreadPins = std::to_array<std::string_view>({
	    "OPENBLAS_NUM_THREADS=1",
	    "OMP_NUM_THREADS=1",
	});

	// Lets compare process perform SIGTERM cleanup without extending scan deadline.
	constexpr auto kCompareTerminationGrace = std::chrono::milliseconds(250);

	using howdy::native::CompareExit;

	auto MakeCompareWaitExitStatus(CompareExit exit_code) -> int {
		return howdy::native::EncodeWaitExitStatus(exit_code);
	}

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
				*status                = MakeCompareWaitExitStatus(CompareExit::kAbort);
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
			              : MakeCompareWaitExitStatus(CompareExit::kAbort);
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
				return MakeCompareWaitExitStatus(CompareExit::kAbort);
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
				return MakeCompareWaitExitStatus(CompareExit::kAbort);
			}
			const unsigned char verdict = record.front();
			const ssize_t       more    = read_record();
			const bool          drained =
			    more == 0 || (more < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
			if (!drained) {
				syslog(LOG_AUTHPRIV | LOG_ERR,
				       "pam_howdy: compare process was reaped elsewhere and reported more than one "
				       "result");
				return MakeCompareWaitExitStatus(CompareExit::kAbort);
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
		using Clock               = std::chrono::steady_clock;
		const int abort_status    = MakeCompareWaitExitStatus(CompareExit::kAbort);
		int       terminal_status = outcome == WaitOutcome::kTimeout
		                                ? MakeCompareWaitExitStatus(CompareExit::kTimeoutReached)
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
		const int abort_status = MakeCompareWaitExitStatus(CompareExit::kAbort);
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

	auto CallPosixSpawnFileActionsInit(void *context, posix_spawn_file_actions_t *actions) -> int {
		(void)context;
		return posix_spawn_file_actions_init(actions);
	}

	auto CallPosixSpawnFileActionsAdddup2(void *context, posix_spawn_file_actions_t *actions,
	                                      int fd, int new_fd) -> int {
		(void)context;
		return posix_spawn_file_actions_adddup2(actions, fd, new_fd);
	}

	auto CallPosixSpawnFileActionsAddclosefrom(void *context, posix_spawn_file_actions_t *actions,
	                                           int from_fd) -> int {
		(void)context;
		return posix_spawn_file_actions_addclosefrom_np(actions, from_fd);
	}

	auto CallPosixSpawnFileActionsDestroy(void *context, posix_spawn_file_actions_t *actions)
	    -> int {
		(void)context;
		return posix_spawn_file_actions_destroy(actions);
	}

	// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
	auto CallSocketpair(void *context, int domain, int type, int protocol, int *socket_fds) -> int {
		(void)context;
		return socketpair(domain, type, protocol, socket_fds);
	}

	auto CallPosixSpawn(const CompareSpawnRequest &request) -> int {
		(void)request.context;
		return posix_spawn(request.child_pid, request.path, request.actions, nullptr,
		                   request.argv.data(), request.envp.data());
	}

	constexpr CompareOperations kPosixSpawnOperations = {
	    .context                   = nullptr,
	    .socketpair                = CallSocketpair,
	    .file_actions_init         = CallPosixSpawnFileActionsInit,
	    .file_actions_adddup2      = CallPosixSpawnFileActionsAdddup2,
	    .file_actions_addclosefrom = CallPosixSpawnFileActionsAddclosefrom,
	    .file_actions_destroy      = CallPosixSpawnFileActionsDestroy,
	    .spawn                     = CallPosixSpawn,
	};

	// Moves `fd` to at least `floor`, unless it is there already. Both verdict socket ends stay
	// above the verdict descriptor: PAM's end is long-lived and Howdy-owned, and compare's end is
	// placed at the verdict descriptor in the child, where a dup2 onto itself would leave it
	// close-on-exec.
	auto RaiseFd(howdy::native::ScopedFd *fd, int floor) -> bool {
		if (fd->Get() >= floor) {
			return true;
		}
		const int raised = fcntl(fd->Get(), F_DUPFD_CLOEXEC, floor);
		if (raised < 0) {
			return false;
		}
		fd->Reset(raised);
		return true;
	}

	// The private socket pair compare reports its verdict on (protocol/compare_verdict.hpp).
	auto OpenVerdictSocket(const CompareOperations &operations, howdy::native::ScopedFd *pam_end,
	                       howdy::native::ScopedFd *compare_end) -> int {
		using howdy::native::compare_verdict::kFd;
		std::array<int, 2> socket_fds = {-1, -1};
		if (operations.socketpair(
		        operations.context, AF_UNIX,
		        howdy::native::CombineBitsAs<int>(SOCK_SEQPACKET, SOCK_CLOEXEC, SOCK_NONBLOCK), 0,
		        socket_fds.data()) != 0) {
			return errno;
		}
		pam_end->Reset(socket_fds.at(0));
		compare_end->Reset(socket_fds.at(1));
		if (!RaiseFd(pam_end, kFd + 1) || !RaiseFd(compare_end, kFd + 1)) {
			const int error_number = errno;
			pam_end->Reset();
			compare_end->Reset();
			return error_number;
		}
		return 0;
	}

	// `verdict_socket_fd` is compare's end of the verdict socket, or -1; `*verdict_used` says
	// whether compare got it, which a failure to add its file action can prevent.
	auto SpawnCompareProcess(const howdy::pam::CompareLaunchRequest &request, pid_t *child_pid,
	                         int verdict_socket_fd, bool *verdict_used,
	                         const CompareOperations &operations) -> int {
		*verdict_used = false;
		std::string compare_path(kCompareProcessPath);
		std::string config_option(howdy::native::kCompareConfigOption);
		std::string config_path(request.config_path);
		std::string username(request.username);

		std::array<char *, 5> args = {
		    compare_path.data(), config_option.data(), config_path.data(), username.data(), nullptr,
		};

		std::string user_models_env =
		    "HOWDY_USER_MODELS_DIR=" + std::string(request.user_models_dir);

		// Compare never calls BLAS, but a threaded BLAS linked into OpenCV starts its thread pool
		// when the library loads and reserves address space per thread (about 128 MiB per OpenMP
		// thread with Arch's OpenBLAS), which can exhaust the compare RLIMIT_AS. The pool is
		// created before main(), so the environment is the only place to limit it. These are fixed
		// values, never copied from the caller. If compare starts using BLAS-heavy OpenCV calls,
		// revisit this.
		auto                pins = std::ranges::to<std::vector<std::string>>(kCompareThreadPins);
		std::vector<char *> environment;
		environment.reserve(pins.size() + 3);
		if (request.staged_runtime) {
			environment.push_back(user_models_env.data());
		}
		for (auto &pin : pins) {
			environment.push_back(pin.data());
		}

		posix_spawn_file_actions_t file_actions;
		const int init_result = operations.file_actions_init(operations.context, &file_actions);
		if (init_result != 0) {
			return init_result;
		}

		// Compare's end of the verdict socket becomes its verdict descriptor, and everything above
		// that is closed, including the end's original descriptor. Without the socket (none, or
		// its action could not be added), compare runs as before: everything above stdio is
		// closed and nothing is announced.
		bool verdict_socket = false;
		if (verdict_socket_fd >= 0) {
			const int dup2_result = operations.file_actions_adddup2(
			    operations.context, &file_actions, verdict_socket_fd,
			    howdy::native::compare_verdict::kFd);
			verdict_socket = dup2_result == 0;
			if (!verdict_socket) {
				syslog(LOG_AUTHPRIV | LOG_WARNING,
				       "pam_howdy: Could not pass the compare verdict socket: %s (%d)",
				       howdy::native::SystemErrorMessage(dup2_result).c_str(), dup2_result);
			}
		}
		std::string verdict_env(howdy::native::compare_verdict::kEnvironmentEntry);
		if (verdict_socket) {
			environment.push_back(verdict_env.data());
		}
		environment.push_back(nullptr);
		const std::span<char *const> compare_env(environment);

		const int closefrom_result = operations.file_actions_addclosefrom(
		    operations.context, &file_actions,
		    verdict_socket ? howdy::native::compare_verdict::kFd + 1 : STDERR_FILENO + 1);
		if (closefrom_result != 0) {
			(void)operations.file_actions_destroy(operations.context, &file_actions);
			return closefrom_result;
		}

		const int spawn_result = operations.spawn({
		    .context   = operations.context,
		    .child_pid = child_pid,
		    .path      = kCompareProcessPath,
		    .actions   = &file_actions,
		    .argv      = args,
		    .envp      = compare_env,
		});
		(void)operations.file_actions_destroy(operations.context, &file_actions);
		*verdict_used = verdict_socket && spawn_result == 0;
		return spawn_result;
	}

}  // namespace

namespace howdy::pam::compare_process {

	auto ProductionOperations() -> CompareOperations {
		return kPosixSpawnOperations;
	}

	auto Spawn(const CompareLaunchRequest &request, pid_t *child_pid, int *child_pidfd,
	           int *verdict_fd, const CompareOperations &operations) -> int {
		if (child_pid == nullptr || child_pidfd == nullptr || verdict_fd == nullptr) {
			return EINVAL;  // the pidfd is required, so there is nowhere to put a child without it
		}
		*child_pidfd = -1;
		*verdict_fd  = -1;
		howdy::native::ScopedFd verdict_pam_end;
		howdy::native::ScopedFd verdict_compare_end;
		// The socket only matters when the application reaps compare first; without it compare
		// still runs, and such a wait fails closed as before.
		if (const int error_number =
		        OpenVerdictSocket(operations, &verdict_pam_end, &verdict_compare_end);
		    error_number != 0) {
			syslog(LOG_AUTHPRIV | LOG_WARNING,
			       "pam_howdy: Could not open the compare verdict socket: %s (%d)",
			       howdy::native::SystemErrorMessage(error_number).c_str(), error_number);
		}
		bool      verdict_used = false;
		const int result       = SpawnCompareProcess(request, child_pid, verdict_compare_end.Get(),
		                                             &verdict_used, operations);
		// Only compare holds its end from here, so its exit ends the socket.
		verdict_compare_end.Reset();
		if (!verdict_used) {
			verdict_pam_end.Reset();
		}
		// Taken before the PAM application can reap compare and let the PID be reused.
		if (result == 0 && *child_pid > 0) {
			// A pidfd opened later could name another process, so a failure fails the spawn.
			int error_number = 0;
			*child_pidfd = howdy::pam::runtime::TakeSpawnedChildPidfd(*child_pid, &error_number);
			if (*child_pidfd < 0) {
				syslog(LOG_AUTHPRIV | LOG_ERR,
				       "pam_howdy: Could not take a pidfd for compare process: %s (%d)",
				       howdy::native::SystemErrorMessage(error_number).c_str(), error_number);
				*child_pid = -1;
				return error_number;
			}
			*verdict_fd = verdict_pam_end.Release();
		}
		return result;
	}

	auto WaitUntil(pid_t child_pid, int child_pidfd, int verdict_fd,
	               std::chrono::steady_clock::time_point deadline, void *cancellation_context,
	               CompareCancellationRequestedFn cancellation_requested, int cancellation_fd)
	    -> int {
		return WaitForCompareProcess(child_pid, child_pidfd, verdict_fd, deadline,
		                             cancellation_context, cancellation_requested, cancellation_fd);
	}

	auto Wait(void *context, pid_t child_pid, int child_pidfd, int verdict_fd,
	          std::chrono::steady_clock::time_point deadline, void *cancellation_context,
	          CompareCancellationRequestedFn cancellation_requested, int cancellation_fd) -> int {
		(void)context;
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
