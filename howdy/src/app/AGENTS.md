# App Guidelines

Parent: [`../../AGENTS.md`](../../AGENTS.md). Related: [`../cli/AGENTS.md`](../cli/AGENTS.md).

## Boundaries

- `howdy_cli_core` (`howdy.cpp`, `howdy/cli.cpp`, `howdy/completion.cpp`, `howdy_entrypoint.cpp`)
  stays independent of concrete command implementations; `howdy_cli` (`howdy/production.cpp` +
  `src/cli/`) owns production composition.
- Keep `src/bin/howdy_main.cpp` as a thin process entrypoint only.
- Keep catalog data and lookup APIs in `command_catalog.cpp` and structural validation in
  `command_catalog/validation.cpp`.
- Tests under `tests/app/` may use `include/app/howdy/internal.hpp` as the shared injection seam.

## Invariants

- Register commands through `command_catalog` and the explicit dispatcher dependency table.
- Keep top-level CLI syntax policy in app dispatch and CLI parsing rather than duplicating it inside
  subcommands.
- Preserve help and exit status behavior: help returns 0, usage errors return 2, and runtime
  failures preserve command-specific diagnostics and exit codes.
