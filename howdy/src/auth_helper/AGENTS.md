# Auth Helper Guidelines

Read [`../../AGENTS.md`](../../AGENTS.md) first.

## Ownership

`runtime.cpp` is the public orchestration façade. The private runtime implementation is split into:

- `runtime/sources.cpp`: secure source/config/model acquisition and source stability
- `runtime/slots.cpp`: staged runtime slot lifecycle, refresh, reuse, and lease handling
- `runtime/internal.hpp`: private cross-TU contract

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
