# CLI Guidelines

Parent: [`../../AGENTS.md`](../../AGENTS.md). Related: [`../app/AGENTS.md`](../app/AGENTS.md).

## Boundaries

- Keep commands single-file until a genuine workflow or process seam emerges.
  `confirmation_prompt.{cpp,hpp}` (shared `[y/N]` prompt for `clear` and `remove`) and
  `privilege_drop.{cpp,hpp}` (permanent drop to the invoking user for `test` and the `config` editor)
  are the only non-command components.
- Dispatcher owns top-level syntax and normalized global options; subcommands receive typed
  invocations and own command-specific semantic validation.
- Keep editor, capture, and environment adapters in production code (`add.cpp`,
  `config/editor_process.cpp`, `snapshot.cpp`, `test.cpp`), outside injected workflow policy.
- `include/cli/<command>/internal.hpp` headers are intentional shared/test injection seams; keep
  test-only helpers under `howdy/tests/include/cli/`.

## Behavior Invariants

- Keep config edits secure and atomic with stale-content detection and temporary-file cleanup.
- Keep `download-models` download, integrity verification, staging, and atomic installation as one
  cohesive workflow; fail closed on missing injected callbacks before network or filesystem work.
- Validate BGR frame batches in the snapshot writer and report commit-sync uncertainty without
  destructive retry.
- Route enrollment capture diagnostics through `ClassifyEnrollmentCaptureFailure()`.
- Drop root for good only through `DropPrivilegesPermanently`, and stop when it fails. It runs in the
  editor child between `fork` and `exec`, so keep it free of messages, logging, and exceptions.
- `howdy test` never probes the invoking user's home or runtime dir as root: probe under that
  user's temporary effective IDs before the permanent drop, and fail closed if switching or
  restoring credentials fails.
