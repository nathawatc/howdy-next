# PAM Runtime Guidelines

**Updated:** 2026-09-06

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
