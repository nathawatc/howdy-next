# PAM Runtime Guidelines

Read [`../../AGENTS.md`](../../AGENTS.md) first.

## Ownership

- `runtime_session.cpp`: one-shot auth-helper staging, typed config load, cleanup
- `auth_helper_process.cpp`: public/high-level helper orchestration
- `auth_helper_process/spawn.cpp`: descriptor and spawn setup
- `auth_helper_process/io.cpp`: bounded I/O, timeout, wait, terminate, reap
- `auth_helper_process/lease.cpp`: SCM_RIGHTS lease transport and validation
- `compare_process.cpp`: compare argv/environment, descriptor closure, timeout/reap

`auth_helper_process/internal.hpp` is private cross-TU plumbing.

## Invariants

Preserve process/FD ownership, EINTR behavior, deadline semantics, timeout termination/reap, helper
output bounds, protocol validation, and lease path/owner/mode/device/inode/flock checks.

`RuntimeSession` is one-shot. Invalid staged paths, missing typed config, or repeated load attempts
fail closed. Direct compare runtime uses an empty environment; staged runtime exports only
`HOWDY_USER_MODELS_DIR`. Keep compare argv behavior unchanged.

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
