# PAM Runtime Guidelines

Read [`../../AGENTS.md`](../../AGENTS.md) first.

## Ownership

- `runtime_session.cpp`: one-shot runtime descriptors (in-process for root, through the helper
  otherwise), their validation, typed config load
- `auth_helper_process.cpp`: helper orchestration and the descriptor-message exchange
- `auth_helper_process/spawn.cpp`: descriptor and spawn setup
- `auth_helper_process/io.cpp`: timeout, wait, terminate, reap
- `compare_process.cpp`: compare argv/environment, descriptor closure, timeout/reap
- `child_process.cpp`: `SpawnChild` and `TrackedChild`, the one spawn and the one pidfd
  wait/signal implementation with PID fallback, used by both child lifecycles

`auth_helper_process/internal.hpp` is private cross-TU plumbing.

## Invariants

Preserve process/FD ownership, EINTR behavior, deadline semantics, timeout termination/reap,
protocol validation, and the runtime-descriptor checks.

A root PAM module opens the runtime config and the user's model in-process with the shared opener
(`auth_helper/runtime_descriptors.hpp`). Any other module runs the setuid helper's `open`, which
trusts only the real UID and therefore cannot serve a root module authenticating someone else.
Either way `RuntimeSession` checks every descriptor before use: close-on-exec, open read-only and
not `O_PATH`, a regular file with one link that only the trusted owner (root in production) can
write, and a model descriptor exactly when the model state is ready. It owns them until the
authentication returns. Compare always runs in descriptor mode (`protocol/compare_descriptors.hpp`).

Signal and reap child processes through a pidfd, never a bare PID: a PAM application that reaps
children itself can free the PID for reuse. Both children start through `SpawnChild`, which uses
`pidfd_spawn` (glibc 2.39, built only where available) so the pidfd comes with the child and no reap
can come in between; `pidfd_getpid` supplies the PID the fallbacks below need, and a PID already
gone reads as -1. Where the build has no `pidfd_spawn`, or it fails with `ENOSYS` (a kernel or
filter without clone3), the child comes from `posix_spawn` and the pidfd is taken right after it.
Carry it to every wait and signal; never open one later from the PID, which may by then name another
process. A pidfd is required: when it cannot be taken, stop the child at once and fail the spawn
(compare has always needed `pidfd_open` to poll, so this costs no configuration that worked before).
The one exception is an auth helper already reaped elsewhere by the time its pidfd or PID is read
(`ESRCH`; `ECHILD` from the ownership check; or `EINVAL`, which older kernels' `pidfd_open` returns
while a reaped process is being released; see `PidfdErrorMeansChildGone`): nothing is left to name
or stop, and its message decides, as below. Compare has no such path; it fails the spawn.
`TrackedChild` never uses a PID at or below 0, so a missing PID can never reach `kill(-1)` or
`waitpid(-1)`. The auth helper keeps the pidfd in `HelperSpawn`; compare hands it back through the
spawn callback, and the prompt coordinator owns it for the run.

`ECHILD` from a pidfd wait, or `ESRCH` from a signal (an exited, unreaped child still accepts
signals), means the child was reaped elsewhere; it is then never signalled or waited on again. Where
a pidfd signal fails later (a filter on the application), the signal falls back to the PID only
after the kernel reports an unreaped child with it; when nothing can confirm that, no signal is
sent. A wait falls back to the PID only on a kernel without `waitid(P_PIDFD)` (`EINVAL`, Linux 5.3);
any other wait failure fails closed. Nothing blocks on a child that could not be signalled.
`TrackedChild` and `KillAndReap` in `child_process.cpp` hold these rules for both children. A PAM
application whose SIGCHLD action auto-reaps (`SIG_IGN` or `SA_NOCLDWAIT`), or that reaps every child
itself, takes the exit status before Howdy can read it. Compare therefore also reports its exit code
as one byte on a private verdict socket (`protocol/compare_verdict.hpp`, an `AF_UNIX`
`SOCK_SEQPACKET` pair): Spawn passes compare's end at fd 3 and announces it in the environment, and
compare checks at startup that fd 3 is a socket before it writes. The wait reads the socket only
after an `ECHILD` from the pidfd wait and accepts exactly one byte; an available exit status always
decides, and no single verdict fails closed. A socket, unlike a pipe, cannot be reopened through
`/proc/<pid>/fd`, so another process cannot add a byte. The verdict socket, config and model are all
required: a socket or file action that cannot be set up fails the spawn. A timeout or cancellation
stays one when compare is gone by the time the wait ends it, whether this wait or the application
reaped it. Compare never changes the application's SIGCHLD action, and its production `main` exits
with `_exit` right after reporting, so nothing runs after the verdict that could still end compare
abnormally.

The auth helper's whole result is one message on its socket
(`protocol/runtime_descriptor_message.hpp`): the marker and model state, plus `SCM_RIGHTS` carrying
the config and, when the model is ready, the model. Its stdio is `/dev/null` and it logs to syslog.
`ReceiveHelperResult` requires exactly that message and then end of file; no message, a second
message, a malformed one, or the deadline fails the open. The exit status is reaped and only logged,
never used to decide, so a host that reaps the helper itself changes nothing. Like compare's verdict
socket, the helper's socket cannot be reopened through `/proc/<pid>/fd`. Neither child changes the
application's SIGCHLD action. Rejected alternatives: changing the action around the spawn, as
`pam_unix` does around `unix_chkpwd`, is process-wide: host children that exit meanwhile stay
zombies, host threads changing the action race with it, and a handler that reaps every child still
takes the status. Spawning with `exit_signal = 0` does not work, because exec resets it to SIGCHLD
(`de_thread` in `fs/exec.c`), and draining zombies with `waitid(P_ALL)` would reap host children
Howdy does not own.
glibc's `<sys/pidfd.h>` tests `_PIDFD_H` but never defines it (seen in 2.44), so a second inclusion
in one unity-build translation unit fails; only `child_process.cpp` includes it.

`RuntimeSession` is one-shot. Rejected descriptors, missing typed config, or repeated load attempts
fail closed. Compare runs with a fixed environment, never copied from the caller: only
`OPENBLAS_NUM_THREADS=1` and `OMP_NUM_THREADS=1`, then `HOWDY_COMPARE_VERDICT_FD=3`,
`HOWDY_COMPARE_CONFIG_FD=4` and `HOWDY_COMPARE_MODEL_FD=5`, with argv `{howdy-compare, user}`. The
child keeps stdio and fds 3 to 5; every descriptor above that is closed. The thread
pins stop a threaded BLAS linked into OpenCV from reserving address space per CPU thread at library
load, which exhausts the compare `RLIMIT_AS` (issues #11, #12). OpenBLAS built with OpenMP (Arch)
reserves about 128 MiB per thread and obeys only `OMP_NUM_THREADS`; pthread builds obey
`OPENBLAS_NUM_THREADS`, so both are pinned. The pool is created before `main()`, so only the
environment can limit it. Compare made no BLAS or LAPACK calls when counted on OpenCV 5.0.0 and
5.1.0 (detector, align, SFace feature; 5.1 runs DNN GEMM through its vendored MLAS, not BLAS), so
the pins cost no BLAS speed; recheck that count when OpenCV changes. Compare also sets OpenCV's
thread count explicitly, which overrides the `OMP_NUM_THREADS` pin for an OpenMP-backend OpenCV.

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
