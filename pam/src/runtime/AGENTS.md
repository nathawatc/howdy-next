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
always needed `pidfd_open` to poll, so this costs no configuration that worked before). The auth
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
glibc's `<sys/pidfd.h>` tests `_PIDFD_H` but never defines it (seen in 2.44), so a second inclusion
in one unity-build translation unit fails; only `child_process.cpp` includes it.

`RuntimeSession` is one-shot. Invalid staged paths, missing typed config, or repeated load attempts
fail closed. Compare runs with a fixed environment, never copied from the caller. Direct runtime
exports only `OPENBLAS_NUM_THREADS=1` and `OMP_NUM_THREADS=1`; staged runtime adds
`HOWDY_USER_MODELS_DIR` first. The thread pins stop a threaded BLAS linked into OpenCV from
reserving address space per CPU thread at library load, which exhausts the compare `RLIMIT_AS`
(issues #11, #12). OpenBLAS built with OpenMP (Arch) reserves about 128 MiB per thread and obeys
only `OMP_NUM_THREADS`; pthread builds obey `OPENBLAS_NUM_THREADS`, so both are pinned. The pool is
created before `main()`, so only the environment can limit it. Compare made no BLAS or LAPACK calls
when counted on OpenCV 5.0.0 and 5.1.0 (detector, align, SFace feature; 5.1 runs DNN GEMM through
its vendored MLAS, not BLAS), so the pins cost no BLAS speed; recheck that count when OpenCV
changes. Compare also sets OpenCV's thread count explicitly, which
overrides the `OMP_NUM_THREADS` pin for an OpenMP-backend OpenCV. Keep compare argv behavior
unchanged.

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
