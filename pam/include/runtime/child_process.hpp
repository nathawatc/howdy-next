#pragma once

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <optional>

#include <spawn.h>

#include <sys/types.h>

namespace howdy::pam::runtime {

	enum class ChildState : std::uint8_t {
		kRunning,
		kReaped,
		kError,
	};

	// Close-on-exec pidfd for `child_pid`, placed above STDERR_FILENO even when the PAM
	// application runs with stdio closed, or -1 with errno set. A pidfd is only as good as the
	// moment it is opened: open it right after spawn, before the PAM application can reap the
	// child and let the PID be reused. Opened later, it may name another process; a pidfd wait
	// reports ECHILD unless that process is also a child of this one.
	[[nodiscard]] __attribute__((visibility("hidden"))) auto
	OpenChildPidfd(pid_t child_pid) noexcept -> int;

	// One child process, signalled and reaped through its pidfd rather than its bare PID. A pidfd
	// names one process for as long as it is open, so from the moment it was opened nothing here
	// can reach an unrelated process that reuses the PID after the PAM application reaped the
	// child itself. Once anything shows that happened (ECHILD from a pidfd wait, or ESRCH from a
	// signal, which a merely exited child never returns), the child is never signalled or waited
	// on again.
	//
	// A pidfd is required: spawns stop the child and fail when they cannot take one (only the auth
	// helper continues when it was already reaped elsewhere; see src/runtime/AGENTS.md). Where a
	// pidfd operation fails later, a signal falls back to the PID only after the kernel reports
	// that a child of this process with that PID is still unreaped, and a wait falls back to the
	// PID only on a kernel without waitid(P_PIDFD) (Linux 5.3); other wait failures fail. That is
	// weaker than the pidfd: it cannot tell the tracked child from another child of the application
	// that reused the PID, which no PID-based call can, and a short window remains between the
	// check and the kill in which the application could reap the child. Constructed with pidfd -1,
	// every operation uses that PID mode. A PID at or below 0 (unknown) is never used: those
	// fallbacks then report the child as reaped elsewhere.
	class __attribute__((visibility("hidden"))) TrackedChild {
	public:
		// `pidfd` is borrowed and may be -1.
		// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
		TrackedChild(pid_t pid, int pidfd) noexcept;

		// Reaps the child, waiting for it to exit when `block` is set. `status` receives a
		// wait(2)-style status once reaped. kError leaves errno set; ECHILD means the child was
		// reaped elsewhere.
		[[nodiscard]] auto Wait(bool block, int *status) noexcept -> ChildState;

		// Zero when the signal was sent or the child has exited and awaits its reap; ECHILD when
		// the child was reaped elsewhere; otherwise the errno of the attempt that decided the
		// failure. After any failure, do not block on the child.
		[[nodiscard]] auto Signal(int signal) noexcept -> int;

		// Returns once the child has exited or `timeout` has passed, without reaping it; true when
		// it saw the exit. Polls the pidfd, so it wakes as soon as the child exits.
		[[nodiscard]] auto AwaitExit(std::chrono::milliseconds timeout) noexcept -> bool;

		// True once a wait or signal showed the child was reaped elsewhere (ECHILD or ESRCH).
		[[nodiscard]] auto WasReapedElsewhere() const noexcept -> bool {
			return reaped_elsewhere_;
		}

	private:
		[[nodiscard]] auto SignalByPid(int signal) noexcept -> int;
		[[nodiscard]] auto ReapedElsewhere() noexcept -> int;

		pid_t pid_;
		int   pidfd_;
		bool  pidfd_wait_usable_;
		bool  reaped_elsewhere_ = false;
	};

	// Stops a child that may still be running: SIGKILL, then a blocking reap only when the signal
	// was delivered, otherwise a single non-blocking attempt, so it never blocks on a child it
	// could not signal. Returns the reaped status, or nullopt when the child was reaped elsewhere
	// or could not be reaped.
	[[nodiscard]] __attribute__((visibility("hidden"))) auto
	KillAndReap(TrackedChild &child) noexcept -> std::optional<int>;

	// Whether an OpenChildPidfd error for a child just spawned means the child is already gone:
	// ESRCH (no such PID), ECHILD (the PID no longer names our child), or EINVAL, which
	// pidfd_open returns for a PID still allocated but with no process left, as while the kernel
	// releases one the application reaped (older kernels; newer ones report ESRCH). With a PID
	// above 0 and no flags, pidfd_open has no other cause for EINVAL.
	[[nodiscard]] constexpr auto PidfdErrorMeansChildGone(int open_error) noexcept -> bool {
		return open_error == ESRCH || open_error == ECHILD || open_error == EINVAL;
	}

	// For a child just spawned when OpenChildPidfd failed with `open_error`. When that means the
	// child is gone (PidfdErrorMeansChildGone), its PID may name another process, so nothing is
	// touched; any other error leaves the child ours, and it is stopped and reaped by PID.
	__attribute__((visibility("hidden"))) void StopChildWithoutPidfd(pid_t child_pid,
	                                                                 int   open_error) noexcept;

	struct SpawnArguments {
		const char                       *path    = nullptr;
		const posix_spawn_file_actions_t *actions = nullptr;
		char *const                      *argv    = nullptr;
		char *const                      *envp    = nullptr;
	};

	// Spawns `path` and returns the posix_spawn error, or 0. Where the C library has pidfd_spawn
	// (glibc 2.39) and the kernel has clone3 (Linux 5.3), the child's pidfd comes from the same
	// call, so no reap by the PAM application can come between the two: `*pidfd` is close-on-exec
	// and above STDERR_FILENO, and `*pid` is the child's PID, or -1 when the child had already
	// exited and been reaped elsewhere by the time the PID was read. Otherwise (an older glibc, or
	// ENOSYS from a kernel or filter without clone3) the child comes from posix_spawn, `*pidfd`
	// stays -1, and the caller takes it with TakeSpawnedChildPidfd.
	// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
	[[nodiscard]] __attribute__((visibility("hidden"))) auto
	SpawnChild(pid_t *pid, int *pidfd, const SpawnArguments &arguments) noexcept -> int;

	// Takes the pidfd for a child just spawned. On failure returns -1, stores the error (never
	// 0) in `*error`, and stops the child when that error leaves it ours.
	[[nodiscard]] __attribute__((visibility("hidden"))) auto
	TakeSpawnedChildPidfd(pid_t child_pid, int *error) noexcept -> int;

}  // namespace howdy::pam::runtime
