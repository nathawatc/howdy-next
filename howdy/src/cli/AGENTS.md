# CLI Guidelines

**Updated:** 2026-09-06

Read [`../../AGENTS.md`](../../AGENTS.md) and [`../app/AGENTS.md`](../app/AGENTS.md) first.

## Layout

Each command exposes only `<command>.cpp` / `include/cli/<command>.hpp` at module scope. Extra
implementation belongs under `src/cli/<command>/`; shared/test-facing seams belong under
`include/cli/<command>/`.

Current multi-file commands:

- `add.cpp`: production adapters and `add_main()`; `add/workflow.cpp`: injected command policy
- `config.cpp`: command façade; `config/edit_session.cpp`: editor/install workflow
- `snapshot.cpp`: production capture/encoding adapters; `snapshot/workflow.cpp`: validation and
  secure writer policy
- `test.cpp`: command/composition; `test/preview_session.cpp` and `test/preview_renderer.cpp`:
  reusable preview behavior

Other commands remain single-file components unless a real boundary appears.

## Behavior

- Dispatcher owns top-level syntax and normalized global options; subcommands validate only their
  normalized argv.
- Use typed runtime config and shared storage/config/model helpers.
- Config edits stay secure and atomic. Preserve stale-content detection and temporary-file cleanup
  semantics.
- `download-models` keeps download, integrity verification, staging, and atomic installation as one
  cohesive workflow; missing injected callbacks fail closed before network/filesystem work.
- Snapshot writer validates BGR frame batches and reports commit-sync uncertainty without
  destructive retry.
- Enrollment diagnostics use `classify_enrollment_capture_failure()`.
- Keep editor/environment behavior in production adapters, not injected test policy.

## Test Seams

Dependency-injection contracts under `include/cli/<command>/internal.hpp` are shared/test-facing by
design. Keep test-only helpers under `tests/include/cli/`.

Inspect `howdy/CMakeLists.txt` before changing a suite; add, config, download-models, snapshot, and
preview tests span multiple translation units.
