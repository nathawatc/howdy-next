# PAM Runtime Guidelines

Read [`../../AGENTS.md`](../../AGENTS.md) first.

## Ownership

- `runtime_session.cpp`: one-shot auth-helper staging, typed config load, cleanup
- `auth_helper_process.cpp`: public/high-level helper orchestration
- `auth_helper_process/spawn.cpp`: descriptor and spawn setup
- `auth_helper_process/io.cpp`: bounded I/O, timeout, wait, terminate, reap
- `auth_helper_process/lease.cpp`: SCM_RIGHTS lease transport and validation
- `compare_process.cpp`: compare argv/environment, descriptor closure, timeout/reap
- `child_process.cpp`: `TrackedChild`, the one pidfd wait/signal implementation with PID fallback,
  used by both child lifecycles

`auth_helper_process/internal.hpp` is private cross-TU plumbing.

## Invariants

Preserve process/FD ownership, EINTR behavior, deadline semantics, timeout termination/reap, helper
output bounds, protocol validation, and lease path/owner/mode/device/inode/flock checks.

Signal and reap child processes through a pidfd, never a bare PID: a PAM application that reaps
children itself can free the PID for reuse. Take the pidfd right after spawn and carry it to every
wait and signal; never open one later from the PID, which may by then name another process. A
pidfd is required: when it cannot be taken, stop the child at once and fail the spawn (compare has
always needed `pidfd_open` to poll, so this costs no configuration that worked before). The one
exception is an auth helper already reaped elsewhere when the pidfd is taken (`ESRCH`; `ECHILD`
from the ownership check; or `EINVAL`, which older kernels' `pidfd_open` returns while a reaped
process is being released; see `PidfdErrorMeansChildGone`): nothing is left to name or stop, and its
completion record decides, as below. Compare has no such path; it fails the spawn. The auth
helper keeps it in `PreparedHelperSpawn`; compare hands it back through the spawn callback, and the
prompt coordinator owns it for the run.

`ECHILD` from a pidfd wait, or `ESRCH` from a signal (an exited, unreaped child still accepts
signals), means the child was reaped elsewhere; it is then never signalled or waited on again.
Where a pidfd signal fails later (a filter on the application), the signal falls back to the PID
only after the kernel reports an unreaped child with it; when nothing can confirm that, no signal
is sent. A wait falls back to the PID only on a kernel without `waitid(P_PIDFD)` (`EINVAL`, Linux
5.3); any other wait failure fails closed. Nothing blocks on a child that could not be
signalled. `TrackedChild` and `KillAndReap` in
`child_process.cpp` hold these rules for both children.
A PAM application whose SIGCHLD action auto-reaps (`SIG_IGN` or `SA_NOCLDWAIT`), or that reaps every
child itself, takes the exit status before Howdy can read it. Compare therefore also reports its
exit code as one byte on a private verdict socket (`protocol/compare_verdict.hpp`, an `AF_UNIX`
`SOCK_SEQPACKET` pair): Spawn passes compare's end at fd 3 and announces it in the environment, and
compare checks at startup that fd 3 is a socket before it writes. The wait reads the socket only
after an `ECHILD` from the pidfd wait and accepts exactly one byte; an available exit status always
decides, and no single verdict fails closed. A socket, unlike a pipe, cannot be reopened through
`/proc/<pid>/fd`, so another process cannot add a byte. If the socket cannot be set up, compare runs
without it, as before. A timeout or cancellation stays one when compare is gone by the time the wait
ends it, whether this wait or the application reaped it. Compare never changes the application's
SIGCHLD action, and its production `main` exits with `_exit` right after reporting, so nothing runs
after the verdict that could still end compare abnormally.

The auth helper ends a successful prepare by sending a one-byte completion record, with no
descriptors, as the last message on the lease socket (`protocol/auth_helper_protocol.hpp`), after the
lease and the stdout paths, and only when PAM asks for it with `HOWDY_AUTH_HELPER_COMPLETION=1` in the
helper's environment: a `pam_howdy.so` loaded before an upgrade never asks and still expects the
socket to end right after the lease. Like compare's verdict socket, the lease socket cannot be
reopened through `/proc/<pid>/fd`, so no other process can add a record. After the helper is gone,
`ReceiveLeaseSocketEnd` accepts a clean end of file or exactly the record followed by one; anything
else fails the prepare. PAM always asks, and the helper ships with the module, so the record is
required: without it the prepare fails, and a helper that stops sending it fails on every host, not
only on those that reap it. When the exit status is still there it must also be 0; when the helper
wait reports `ECHILD` (`kReapedElsewhere`), or the helper was already reaped when its pidfd was to
be taken, the record alone shows success. Those reaped-elsewhere notes log at `LOG_DEBUG`, as they
are routine on such hosts. Neither
child changes the application's SIGCHLD action. Rejected alternatives: changing the action around
the spawn, as `pam_unix` does around `unix_chkpwd`, is process-wide: host children that exit
meanwhile stay zombies, host threads changing the action race with it, and a handler that reaps
every child still takes the status. Spawning with `exit_signal = 0` does not work, because exec
resets it to SIGCHLD (`de_thread` in `fs/exec.c`), and draining zombies with `waitid(P_ALL)` would
reap host children Howdy does not own.
glibc's `<sys/pidfd.h>` tests `_PIDFD_H` but never defines it (seen in 2.44), so a second inclusion
in one unity-build translation unit fails; only `child_process.cpp` includes it.

`RuntimeSession` is one-shot. Invalid staged paths, missing typed config, or repeated load attempts
fail closed. Compare runs with a fixed environment, never copied from the caller. Direct runtime
exports only `OPENBLAS_NUM_THREADS=1` and `OMP_NUM_THREADS=1`, plus `HOWDY_COMPARE_VERDICT_FD=3`
when the verdict socket was set up and passed; staged runtime adds `HOWDY_USER_MODELS_DIR` first.
The child keeps stdio and, with the socket, fd 3; every descriptor above that is closed. The thread
pins stop a threaded BLAS linked into OpenCV from reserving address space per CPU thread at library
load, which exhausts the compare `RLIMIT_AS` (issues #11, #12). OpenBLAS built with OpenMP (Arch)
reserves about 128 MiB per thread and obeys only `OMP_NUM_THREADS`; pthread builds obey
`OPENBLAS_NUM_THREADS`, so both are pinned. The pool is created before `main()`, so only the
environment can limit it. Compare made no BLAS or LAPACK calls when counted on OpenCV 5.0.0 and
5.1.0 (detector, align, SFace feature; 5.1 runs DNN GEMM through its vendored MLAS, not BLAS), so
the pins cost no BLAS speed; recheck that count when OpenCV changes. Compare also sets OpenCV's
thread count explicitly, which overrides the `OMP_NUM_THREADS` pin for an OpenMP-backend OpenCV.
Keep compare argv behavior unchanged.

Do not fragment `auth_helper_process` further for LOC; its process lifecycle is intentionally one
auditable subsystem.

## Known Limitation: SSH Detection (Won't Fix)

`session_probe.cpp` (`abort_if_ssh`) only checks `SSH_CONNECTION`, `SSH_CLIENT`, `SSH_TTY`, and
`SSHD_OPTS` in the PAM and process environment. The caller controls these, so an SSH user can run
`env -u SSH_CONNECTION -u SSH_CLIENT -u SSH_TTY sudo ...` and get a face check on the local camera.
Treat `abort_if_ssh` as a convenience, not a security boundary.

This is intentionally unfixed. A logind (sd-login) approach was tried and dropped. Many legitimate
callers run outside any logind session (GUI terminals, the gdm worker, the socket-activated polkit
helper), and so does `systemd-run --user`, so no logind signal is exact. Every heuristic either left
a bypass (`su` to another user, lingering processes of an ended SSH session, sshd/xrdp stacks before
a session exists, tmux started locally) or disabled local face unlock while any SSH session was
open. Do not reopen this without a process-ancestry design that addresses all of those cases.
