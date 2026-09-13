# App Guidelines

**Updated:** 2026-09-06

Read [`../../AGENTS.md`](../../AGENTS.md) first.

## Ownership

- `howdy.cpp`: top-level command dispatch, target-user/root policy, command execution
- `howdy/production.cpp`: production user/privilege adapters and command entrypoint wiring
- `howdy/cli.cpp`: argv parsing, validation, help, usage errors
- `howdy/completion.cpp`: hidden completion protocol
- `command_catalog.cpp`: command/global-option data and lookup façade
- `command_catalog/validation.cpp`: catalog structural validation
- `src/bin/howdy_main.cpp`: process entrypoint only

## Rules

- Add commands through the catalog and dispatcher dependency table; do not create standalone
  executables.
- Keep catalog data and lookup APIs in `command_catalog.cpp`; validation belongs in
  `command_catalog/validation.cpp`.
- `command_catalog/internal.hpp` is private cross-TU plumbing, not a public/test seam.
- Keep CLI syntax policy in app dispatch/CLI parsing, not duplicated inside unrelated commands.
- Preserve help/status behavior: help succeeds; usage errors use status 2; runtime failures keep
  command-specific diagnostics/status.

Tests for dispatcher/completion live under `tests/app/` and may use `include/app/howdy/internal.hpp`
for the shared injection seam.
