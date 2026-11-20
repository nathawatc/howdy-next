#ifndef _GNU_SOURCE
#	define _GNU_SOURCE
#endif

#include "runtime/child_process.hpp"

#include "support/bit_flags.hpp"
#include "support/eintr.hpp"
#include "support/process_status.hpp"

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <optional>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <sys/poll.h>
#include <sys/types.h>
#include <sys/wait.h>

// glibc's <sys/pidfd.h> tests _PIDFD_H but never defines it (seen in 2.44), so a second inclusion
// in one translation unit, which a unity build creates, redefines struct pidfd_info; this is the
// only file that includes it. Keep C linkage
// explicit: the header also lacks a C++ linkage guard.
extern "C" {
#include <sys/pidfd.h>
}

namespace {

	constexpr unsigned int kExitCodeMask = 0xffU;

	// waitid reports the outcome in pieces; callers use the classic WIFEXITED/WIFSIGNALED form.
	auto WaitStatusFromSiginfo(const siginfo_t &info) noexcept -> int {
		if (info.si_code == CLD_EXITED) {
			return howdy::native::EncodeWaitExitStatus(static_cast<unsigned int>(info.si_status) &
			                                           kExitCodeMask);
		}
		return howdy::native::EncodeWaitSignalStatus(info.si_status, info.si_code == CLD_DUMPED);
	}

	enum class ChildPeek : std::uint8_t {
		kRunning,
		kExited,       // exited and not yet reaped
		kNotOurs,      // ECHILD: reaped elsewhere, or never a child of this process
		kUnavailable,  // the peek itself failed; errno is set
	};

	// Looks at a child's state without reaping it.
	auto PeekChild(idtype_t id_type, id_t id) noexcept -> ChildPeek {
		siginfo_t info{};
		if (howdy::native::RetryOnEintr([&] -> int {
			    info = {};
			    return waitid(id_type, id, &info, WEXITED | WNOHANG | WNOWAIT);
		    }) != 0) {
			return errno == ECHILD ? ChildPeek::kNotOurs : ChildPeek::kUnavailable;
		}
		return info.si_pid != 0 ? ChildPeek::kExited : ChildPeek::kRunning;
	}

	// Peeks through the pidfd while pidfd waits work, which names the child for certain, and by
	// PID otherwise.
	auto PeekTracked(bool pidfd_wait_usable, int pidfd, pid_t pid) noexcept -> ChildPeek {
		return pidfd_wait_usable ? PeekChild(P_PIDFD, static_cast<id_t>(pidfd))
		                         : PeekChild(P_PID, static_cast<id_t>(pid));
	}

}  // namespace

namespace howdy::pam::runtime {

	auto OpenChildPidfd(pid_t child_pid) noexcept -> int {
		int pidfd = pidfd_open(child_pid, 0);
		if (pidfd >= 0 && pidfd <= STDERR_FILENO) {
			// Keep the long-lived descriptor clear of stdio.
			const int moved = fcntl(pidfd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
			// A descriptor limit at or below the target makes this EINVAL, which callers would
			// read as a child already gone (PidfdErrorMeansChildGone); report the limit instead.
			const int error = errno == EINVAL ? EMFILE : errno;
			(void)close(pidfd);
			errno = error;
			pidfd = moved;
		}
		if (pidfd < 0) {
			return -1;
		}
		// If the PID was reaped and reused before the open, the pidfd may name a process that is
		// not our child; a pidfd wait reports that as ECHILD. Linux 5.3 has no P_PIDFD (EINVAL),
		// so there the PID is checked instead, which still rules out a process that is not a
		// child of this one.
		auto peek = PeekChild(P_PIDFD, static_cast<id_t>(pidfd));
		if (peek == ChildPeek::kUnavailable && errno == EINVAL) {
			peek = PeekChild(P_PID, static_cast<id_t>(child_pid));
		}
		if (peek == ChildPeek::kNotOurs) {
			(void)close(pidfd);
			errno = ECHILD;
			return -1;
		}
		return pidfd;
	}

	// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
	TrackedChild::TrackedChild(pid_t pid, int pidfd) noexcept
	    : pid_(pid)
	    , pidfd_(pidfd)
	    , pidfd_wait_usable_(pidfd >= 0) {}

	auto TrackedChild::ReapedElsewhere() noexcept -> int {
		reaped_elsewhere_ = true;
		errno             = ECHILD;
		return ECHILD;
	}

	auto TrackedChild::Wait(bool block, int *status) noexcept -> ChildState {
		if (status == nullptr) {
			errno = EINVAL;
			return ChildState::kError;
		}
		if (reaped_elsewhere_) {
			errno = ECHILD;
			return ChildState::kError;
		}
		if (pidfd_wait_usable_) {
			siginfo_t info{};
			const int options = block ? WEXITED : WEXITED | WNOHANG;
			if (howdy::native::RetryOnEintr([&] -> int {
				    info = {};
				    return waitid(P_PIDFD, static_cast<id_t>(pidfd_), &info, options);
			    }) == 0) {
				// WNOHANG with no state change succeeds and leaves si_pid zero.
				if (info.si_pid == 0) {
					return ChildState::kRunning;
				}
				*status = WaitStatusFromSiginfo(info);
				return ChildState::kReaped;
			}
			if (errno == ECHILD) {
				(void)ReapedElsewhere();
				return ChildState::kError;
			}
			// Only a kernel without P_PIDFD (EINVAL, Linux 5.3) moves the wait to the PID. Any
			// other failure, such as a filter on the PAM application, fails the wait instead of
			// risking a waitpid that reaps another child which reused the PID.
			if (errno != EINVAL) {
				return ChildState::kError;
			}
			pidfd_wait_usable_ = false;
		}
		// Without a PID (the child was already gone when it was read), waitpid would take any
		// child of the application.
		if (pid_ <= 0) {
			(void)ReapedElsewhere();
			return ChildState::kError;
		}
		const pid_t result = howdy::native::RetryOnEintr([&] -> pid_t {
			return waitpid(pid_, status, block ? 0 : WNOHANG);
		});
		if (result == pid_) {
			return ChildState::kReaped;
		}
		if (result == 0) {
			return ChildState::kRunning;
		}
		if (errno == ECHILD) {
			(void)ReapedElsewhere();
		}
		return ChildState::kError;
	}

	// A signal to an exited but unreaped child succeeds, so ESRCH means the child is gone and
	// its PID may already name another process.
	auto TrackedChild::Signal(int signal) noexcept -> int {
		if (reaped_elsewhere_) {
			return ECHILD;
		}
		if (pidfd_ >= 0) {
			if (pidfd_send_signal(pidfd_, signal, nullptr, 0) == 0) {
				return 0;
			}
			if (errno == ESRCH) {
				return ReapedElsewhere();
			}
		}
		return SignalByPid(signal);
	}

	// Peeks without reaping: a child that already exited needs no signal, and one that is no
	// longer ours must not get one. The peek goes through the pidfd when it can, which names the
	// child for certain. When the peek itself is unavailable, for example filtered by the PAM
	// application, nothing confirms the PID still names the child, so no signal is sent: compare
	// and the helper end on their own timeouts, while a signal could reach an unrelated process.
	auto TrackedChild::SignalByPid(int signal) noexcept -> int {
		// Without a PID there is nothing to fall back to; kill(-1) would reach every process.
		if (pid_ <= 0) {
			return ReapedElsewhere();
		}
		switch (PeekTracked(pidfd_wait_usable_, pidfd_, pid_)) {
			case ChildPeek::kNotOurs:
				return ReapedElsewhere();
			case ChildPeek::kExited:
				return 0;
			case ChildPeek::kUnavailable:
				return errno;
			case ChildPeek::kRunning:
				break;
		}
		if (kill(pid_, signal) == 0) {
			return 0;
		}
		return errno == ESRCH ? ReapedElsewhere() : errno;
	}

	auto TrackedChild::AwaitExit(std::chrono::milliseconds timeout) noexcept -> bool {
		using Clock           = std::chrono::steady_clock;
		const auto deadline   = Clock::now() + timeout;
		bool       poll_pidfd = pidfd_ >= 0;
		while (!reaped_elsewhere_) {
			const auto remaining =
			    std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now()).count();
			if (remaining <= 0) {
				return false;
			}
			if (poll_pidfd) {
				pollfd    descriptor{.fd = pidfd_, .events = POLLIN, .revents = 0};
				const int result = poll(&descriptor, 1, static_cast<int>(remaining));
				if (result > 0 && howdy::native::HasAnyBits(descriptor.revents, POLLIN, POLLHUP)) {
					return true;  // exited
				}
				if (result == 0 || (result < 0 && errno == EINTR)) {
					continue;
				}
				// The pidfd cannot be polled; check by PID between short sleeps instead, so a
				// caller that loops on this never spins.
				poll_pidfd = false;
				continue;
			}
			switch (PeekTracked(pidfd_wait_usable_, pidfd_, pid_)) {
				case ChildPeek::kExited:
					return true;
				case ChildPeek::kNotOurs:
					(void)ReapedElsewhere();
					return false;
				case ChildPeek::kRunning:
				case ChildPeek::kUnavailable:
					break;  // an unavailable peek just waits out the timeout
			}
			constexpr int pid_poll_interval_ms = 10;
			(void)poll(nullptr, 0,
			           remaining < pid_poll_interval_ms ? static_cast<int>(remaining)
			                                            : pid_poll_interval_ms);
		}
		return false;
	}

	auto KillAndReap(TrackedChild &child) noexcept -> std::optional<int> {
		int status = 0;
		switch (child.Wait(false, &status)) {
			case ChildState::kReaped:
				return status;
			case ChildState::kError:
				if (errno == ECHILD) {
					return std::nullopt;
				}
				break;  // a failed wait proves nothing; the child may still be running
			case ChildState::kRunning:
				break;
		}
		const bool signalled = child.Signal(SIGKILL) == 0;
		if (child.Wait(signalled, &status) == ChildState::kReaped) {
			return status;
		}
		return std::nullopt;
	}

	// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
	void StopChildWithoutPidfd(pid_t child_pid, int open_error) noexcept {
		if (PidfdErrorMeansChildGone(open_error)) {
			return;
		}
		TrackedChild child(child_pid, -1);
		(void)KillAndReap(child);
	}

	// HOWDY_HAVE_PIDFD_SPAWN comes from the build (CMakeLists.txt) when the C library has
	// pidfd_spawn and pidfd_getpid (glibc 2.39); without it only the posix_spawn path is built.
	// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
	auto SpawnChild(pid_t *pid, int *pidfd, const SpawnArguments &arguments) noexcept -> int {
		*pid   = -1;
		*pidfd = -1;
#ifdef HOWDY_HAVE_PIDFD_SPAWN
		int       spawned_pidfd = -1;
		const int result = pidfd_spawn(&spawned_pidfd, arguments.path, arguments.actions, nullptr,
		                               arguments.argv, arguments.envp);
		if (result != ENOSYS) {
			if (result != 0) {
				return result;
			}
			// -1 (ESRCH) when the child already exited and the PAM application reaped it; the
			// pidfd still names it.
			const pid_t child_pid = pidfd_getpid(spawned_pidfd);
			if (spawned_pidfd <= STDERR_FILENO) {
				// Keep the long-lived descriptor clear of stdio, as OpenChildPidfd does.
				const int moved = fcntl(spawned_pidfd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
				if (moved < 0) {
					const int move_error = errno;
					// The child is ours and still named by this pidfd: stop it before letting go.
					TrackedChild child(child_pid, spawned_pidfd);
					(void)KillAndReap(child);
					(void)close(spawned_pidfd);
					return move_error;
				}
				(void)close(spawned_pidfd);
				spawned_pidfd = moved;
			}
			*pid   = child_pid;
			*pidfd = spawned_pidfd;
			return 0;
		}
#endif
		return posix_spawn(pid, arguments.path, arguments.actions, nullptr, arguments.argv,
		                   arguments.envp);
	}

	auto TakeSpawnedChildPidfd(pid_t child_pid, int *error) noexcept -> int {
		const int pidfd = OpenChildPidfd(child_pid);
		if (pidfd >= 0) {
			return pidfd;
		}
		const int open_error = errno != 0 ? errno : EAGAIN;
		if (error != nullptr) {
			*error = open_error;
		}
		StopChildWithoutPidfd(child_pid, open_error);
		return -1;
	}

}  // namespace howdy::pam::runtime
