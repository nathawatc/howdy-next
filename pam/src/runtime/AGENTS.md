# PAM Runtime Guidelines

Parent: [`../../AGENTS.md`](../../AGENTS.md). Child:
[`auth_helper_process/AGENTS.md`](auth_helper_process/AGENTS.md). Related:
[`../../../howdy/src/compare/AGENTS.md`](../../../howdy/src/compare/AGENTS.md).

## Runtime Session

- A root PAM module opens config and user model in-process via `auth_helper/runtime_descriptors.hpp`
  (the setuid helper trusts only the real UID and cannot serve a root module authenticating another
  user). Non-root PAM runs the setuid helper (`auth_helper_process/AGENTS.md`).
- `RuntimeSession` is one-shot and fails closed on repeated load attempts, missing typed config, or
  rejected descriptors. Before use, verify every descriptor is `O_CLOEXEC`, open read-only and
  non-`O_PATH`, a regular file with `st_nlink == 1` writable only by the trusted owner (root in
  production), and has a model descriptor iff model state is ready. Own them until auth returns.

## Child Processes

- Start both children through `SpawnChild` and wait/signal through `TrackedChild` (shared production
  adapters in `child_process/internal.hpp`).
- Signal and reap through a pidfd acquired at spawn (never opened later from a PID, and never use
  PID <= 0). Require a pidfd on spawn; probe/fallback rules and why Howdy neither changes `SIGCHLD`
  nor calls `waitid(P_ALL)` are commented in `child_process.cpp`.
- Treat `ECHILD` from pidfd wait or `ESRCH` from signal as reaped elsewhere: never signal or wait
  again, and never block on a child that could not be signalled.
- Compare reports its exit code as one byte on the required private verdict socket
  (`protocol/compare_verdict.hpp`). Read it only after `ECHILD` (an available exit status always
  decides; no single verdict fails closed). Keep a timeout or cancellation classified as such even
  if compare is already gone when wait ends it.

## Compare Environment

- Launch compare (`compare_process/spawn.cpp`) with a fixed environment, never inherited:
  `OPENBLAS_NUM_THREADS=1`, `OMP_NUM_THREADS=1` (both pins required; rationale in `spawn.cpp`), plus
  the `protocol/compare_verdict.hpp` and `protocol/compare_descriptors.hpp` entries (`fds 3..5`),
  `argv {howdy-compare, user}`, closing every descriptor above 5.

## Known Limitation: SSH Detection (Won't Fix)

`abort_if_ssh` is a convenience, not a security boundary, and its limitations are a settled
contract; read the bypass, rejected logind approach, and reopening criteria in `session_probe.cpp`
before proposing changes.
