# Howdy Test Guidelines

Parent: [`../AGENTS.md`](../AGENTS.md). Read the matching `src/<subsystem>/AGENTS.md` before
changing tests for a subsystem.

## Rules

- Prefer injected filesystem, clock, capture, renderer, process, and inference dependencies over
  host side effects.
- Lock security-sensitive code with table-driven tests covering success, full malformed input
  classes across sibling entry points, partial failure, rollback/cleanup, and uncertain-commit
  (`kDurabilityUncertain`) paths.
- Protocol decoders (`tests/protocol/runtime_descriptor_message_fuzz*.cpp`) and the INI editor
  (`tests/config/config_key_file_fuzz*.cpp`, differential against inih) have fuzz targets: fixed-input
  replay runs in every build; libFuzzer targets build automatically when `HOWDY_ENABLE_FUZZING`
  (`AUTO`) finds Clang libFuzzer.
