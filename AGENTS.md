# Repository Guidelines

Standing rules for every change. Read the nearest nested `AGENTS.md` before editing a subsystem;
local guides add detail without relaxing these invariants.

## Map

- [`howdy/AGENTS.md`](howdy/AGENTS.md): native CLI, helpers, config, storage, vision, compare, and
  tests
- [`pam/AGENTS.md`](pam/AGENTS.md): PAM module, prompt handling, process runtime, translations, and
  tests
- [`ci/archlinux/AGENTS.md`](ci/archlinux/AGENTS.md): Arch Linux CI container, build workflow, and
  clang-tidy workflow
- [`ci/nix/AGENTS.md`](ci/nix/AGENTS.md): Nix CI container, flake, `nix/` derivations, and Nix
  workflow

## Build and CMake

- Target CMake 3.31+ and C++23.
- Resolve every pkg-config dependency through `howdy_pkg_check_modules()`
  (`cmake/howdy_pkg_config.cmake`) rather than `pkg_check_modules()` directly (header explains the
  CMake `Requires` bug).
- A CTest logical suite may span multiple translation units; inspect `howdy/cmake/tests/*.cmake` or
  `pam/cmake/tests/*.cmake` before assuming one file owns a suite.

## Code and Layout

- Use `snake_case` for files, functions, and tests; preserve tabs in C/C++ (`.clang-format`,
  `.editorconfig`).
- Expose one module-level pair per production component (`src/<module>/<component>.cpp` +
  `include/<module>/<component>.hpp`). Place subordinate implementation under
  `src/<module>/<component>/`, private cross-TU contracts at
  `src/<module>/<component>/internal.hpp`, and shared/test-facing contracts under
  `include/<module>/<component>/` only when external or test consumers need them. Omit empty mirror
  directories. `src/bin/` and `src/tools/` are entrypoint/tool exceptions.
- Keep test-only shared headers under `tests/include/` (package-local or repo-root); keep test
  include roots out of production targets.
- Prefer explicit dependency tables and callback structs over registries, DI containers, or
  virtual-interface frameworks.
- Comment non-obvious *why* inline next to the code (rejected alternatives, security/ordering
  constraints, upstream workarounds) instead of narrating *what* code does or duplicating code-local
  rationale in `AGENTS.md`.
- Update the nearest `AGENTS.md` (and any affected `.agents/skills/*/SKILL.md`) in the same change
  whenever invariants, subsystem boundaries, or workflows change.

## Security

- Fail closed on unexpected errors, malformed inputs, or privilege/ownership violations. Preserve
  privilege boundaries, ownership/mode checks, descriptor lifetimes, lock ordering, staged-file
  semantics, and durability handling; treat post-rename commit-sync failure as potentially
  committed.
- Use typed `RuntimeConfig`, `VideoConfig`, and `FaceConfig` at runtime boundaries; keep raw
  `ConfigReader` lookups inside config parsing/loading.

## Commits

Use Conventional Commits with imperative subjects (`fix(pam): ...`, `refactor(storage): ...`,
`test(config): ...`, `build(release): ...`). Cap subject lines at 72 characters (GitHub truncates
longer subjects). Push only when explicitly requested.
