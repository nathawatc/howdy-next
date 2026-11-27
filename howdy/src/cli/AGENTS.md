# CLI Guidelines

Parent: [`../../AGENTS.md`](../../AGENTS.md). Related: [`../app/AGENTS.md`](../app/AGENTS.md).

## Boundaries

- Keep commands single-file until a genuine workflow or process seam emerges.
  `confirmation_prompt.{cpp,hpp}` (shared `[y/N]` prompt for `clear` and `remove`) and
  `privilege_drop.{cpp,hpp}` (permanent drop to the invoking user for `test` and the `config` editor)
  are the only non-command components.
- Subcommands receive typed invocations from app dispatch and own command-specific semantic
  validation.
- Keep editor, capture, and environment adapters in production code (`add.cpp`,
  `config/editor_process.cpp`, `snapshot.cpp`, `test.cpp`), outside injected workflow policy.
- `include/cli/<command>/internal.hpp` headers are intentional shared/test injection seams.

## Behavior Invariants

- Keep config edits secure and atomic with stale-content detection and temporary-file cleanup. Keep
  the temporary file and print its path whenever uninstalled edits may remain (including failed or
  killed editors). Retain only a regular file owned by the editing user and remove anything else at
  the path—except when the editor's exit status is lost (it may still be running), where the path is
  always kept.
- Keep `download-models` download, integrity verification, staging, and atomic installation as one
  cohesive workflow; fail closed on missing injected callbacks before network or filesystem work.
- Validate BGR frame batches in the snapshot writer and report commit-sync uncertainty without
  destructive retry.
- Route enrollment capture diagnostics through `ClassifyEnrollmentCaptureFailure()`.
- Drop root for good only through `DropPrivilegesPermanently`, and stop when it fails.
- `howdy test` never probes the invoking user's home or runtime dir as root: probe under that
  user's temporary effective IDs before the permanent drop, and fail closed if switching or
  restoring credentials fails.
