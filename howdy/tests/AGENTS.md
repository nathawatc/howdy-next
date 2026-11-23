# Howdy Test Guidelines

Parent: [`../AGENTS.md`](../AGENTS.md). Read the matching `src/<subsystem>/AGENTS.md` before
changing tests for a subsystem.

## Rules

- `howdy/cmake/tests/*.cmake` owns logical CTest suites (several span multiple `.cpp` files);
  inspect before moving or adding coverage.
- Place package test-only shared headers under `howdy/tests/include/<module>/` (cross-package
  helpers live in repo-root `tests/include/`).
- Prefer injected filesystem, clock, capture, renderer, process, and inference dependencies over
  host side effects.
- Cover success, malformed input, partial failure, rollback/cleanup, and uncertain-commit paths on
  security-sensitive code.
- Protocol decoders have fuzz targets (`tests/protocol/runtime_descriptor_message_fuzz*.cpp`):
  fixed-input replay runs in every build; libFuzzer targets build automatically when
  `HOWDY_ENABLE_FUZZING` (`AUTO`) finds Clang libFuzzer.
