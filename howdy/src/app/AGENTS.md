# App Guidelines

**Updated:** 2026-09-13

Read [`../../AGENTS.md`](../../AGENTS.md) first.

## Ownership

- `howdy.cpp`: pure top-level dispatch and target-user/root policy through injected dependencies
- `howdy/production.cpp`: explicit production user/privilege adapters and concrete command binding
- `howdy/cli.cpp`: argv parsing, validation, help, usage errors
- `howdy/completion.cpp`: hidden completion protocol
- `command_catalog.cpp`: command/global-option data and lookup façade
- `command_catalog/validation.cpp`: catalog structural validation
- `src/bin/howdy_main.cpp`: process entrypoint only

## Rules

- `howdy_cli_core` must not depend on concrete command implementations; `howdy_cli` owns production
  composition and command infrastructure.
- Add commands through the catalog and explicit dispatcher dependency table; do not create standalone
  executables, registries, DI containers, or abstract interface frameworks.
- Keep catalog data and lookup APIs in `command_catalog.cpp`; validation belongs in
  `command_catalog/validation.cpp`.
- `command_catalog/internal.hpp` is private cross-TU plumbing, not a public/test seam.
- Keep CLI syntax policy in app dispatch/CLI parsing, not duplicated inside unrelated commands.
- Preserve help/status behavior: help succeeds; usage errors use status 2; runtime failures keep
  command-specific diagnostics/status.

Tests for dispatcher/completion live under `tests/app/` and may use `include/app/howdy/internal.hpp`
for the shared injection seam.
