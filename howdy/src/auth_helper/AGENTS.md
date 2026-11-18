# Auth Helper Guidelines

Read [`../../AGENTS.md`](../../AGENTS.md) first.

## Ownership

`runtime.cpp` is the public orchestration façade. The private runtime implementation is split into:

- `runtime/sources.cpp`: secure source/config/model acquisition and source stability
- `runtime/slots.cpp`: staged runtime slot lifecycle, refresh, reuse, and lease handling
- `runtime/internal.hpp`: private cross-TU contract

`staged_copies.cpp` is the entry point other commands use to purge a user's staged copies; it
resolves the UID and reports what could not be removed. `howdy_auth_helper_runtime` holds the
staging runtime and this entry point without the setuid helper's `command.cpp`.

`include/auth_helper/runtime/internal.hpp` is a separate shared/test-facing contract. Do not merge
it with the private header.

## Security Invariants

Preserve this high-level order: open/validate runtime root → allocation lock → secure config source
→ config FD validation → optional model readiness/source open → slot open → fresh shared lease or
slot refresh.

Keep descriptor-relative checks, ACL/mode/owner validation, source-stability checks, generation-slot
locking, lease identity, and fail-closed invalid-state handling. A healthy slot is updated in
place; a missing slot, or one whose contents fail validation, is rebuilt in its side directory
and swapped in with `renameat2`, never repaired object by object. A slot directory or lock that
itself fails validation is never replaced. Staging runs with umask 0 and effective group root
so the runtime root and lock files are valid the moment they appear; anything that also needs
an ACL is created only inside a side directory. Do not split `runtime/slots.cpp`
merely for LOC; its slot lifecycle is intentionally cohesive.

`PurgeRuntimeAuthFiles` removes one user's staged slots after `howdy remove` or `howdy clear`, so a
removed model does not stay readable until the next prepare. It follows the prepare invariants: it
never creates the runtime root, validates every lock and slot directory before use and leaves one
that fails validation in place, never removes a leased slot or one whose lock file is missing, and
keeps the lock files. A slot is renamed to its side name before removal so it disappears in one
step. It waits about one second for the allocation lock so a passing helper run does not block it; a
retry from a command that changed nothing makes a single attempt.
A slot kept because it is leased is not guaranteed to be rewritten later; callers must not promise
that. `howdy remove` and `howdy clear` purge again when they find no models, which is the retry.
