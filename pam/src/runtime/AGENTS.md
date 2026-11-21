# PAM Runtime Guidelines

Read [`../../AGENTS.md`](../../AGENTS.md) first. The auth helper's spawn, wait, and result rules are
in [`auth_helper_process/AGENTS.md`](auth_helper_process/AGENTS.md).

## Ownership

- `runtime_session.cpp`: one-shot runtime descriptors (in-process for root, through the helper
  otherwise), their validation, typed config load
- `auth_helper_process.cpp`: helper orchestration and the descriptor-message exchange
  (`auth_helper_process/`: spawn and wait, see its guide)
- `compare_process.cpp`: compare argv/environment, descriptor closure, timeout/reap
- `child_process.cpp`: `SpawnChild` and `TrackedChild`, the one spawn and the one pidfd
  wait/signal implementation with PID fallback, used by both child lifecycles
- `session_probe.cpp`: SSH detection, used when the `abort_if_ssh` option is set (eligibility is in
  `module/auth_eligibility.cpp`); `lid_probe.cpp`: lid state;
  `environment.cpp`: PAM and process environment lookup; `message_locale.cpp`: PAM message locale

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

`RuntimeSession` is one-shot. Rejected descriptors, missing typed config, or repeated load attempts
fail closed.

### Child processes

Both children start through `SpawnChild` and are waited on and signalled through `TrackedChild`.

- Signal and reap through a pidfd, never a bare PID: a PAM application that reaps children itself
  can free the PID for reuse. The pidfd comes with the child, is carried to every wait and signal,
  and is never opened later from the PID. A pidfd is required: without one the spawn fails and the
  child is stopped. The probe and fallback rules are commented in `child_process.cpp`.
- `TrackedChild` never uses a PID at or below 0.
- `ECHILD` from a pidfd wait, or `ESRCH` from a signal, means the child was reaped elsewhere: it is
  never signalled or waited on again. Nothing blocks on a child that could not be signalled.
- Neither child changes the application's SIGCHLD action, and nothing drains children with
  `waitid(P_ALL)`. The reasons are commented in `child_process.cpp`.
- Compare reports its exit code as one byte on a private verdict socket
  (`protocol/compare_verdict.hpp`), which the wait reads only after `ECHILD`. An available exit
  status always decides; no single verdict fails closed. The socket is required: a setup failure
  fails the spawn. Compare's production `main` exits with `_exit` right after reporting.
- A timeout or cancellation stays one when compare is gone by the time the wait ends it.

### Compare environment

Compare runs with a fixed environment, never copied from the caller: only `OPENBLAS_NUM_THREADS=1`
and `OMP_NUM_THREADS=1`, then `HOWDY_COMPARE_VERDICT_FD=3`, `HOWDY_COMPARE_CONFIG_FD=4` and
`HOWDY_COMPARE_MODEL_FD=5`, with argv `{howdy-compare, user}`. The child keeps stdio and fds 3 to 5;
every descriptor above that is closed. Both thread pins are required; their reasons are commented in
`compare_process.cpp`.

## Known Limitation: SSH Detection (Won't Fix)

The `abort_if_ssh` option is a convenience, not a security boundary. The bypass, the rejected logind approach,
and the conditions for reopening the question are documented in `session_probe.cpp`. Read them
before changing SSH detection.
