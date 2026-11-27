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

- Target CMake 3.31+ and C++23; build and test with `cmake --preset debug|release` and
  `ctest --preset debug|release`.
- Resolve every pkg-config dependency through `howdy_pkg_check_modules()`
  (`cmake/howdy_pkg_config.cmake`) rather than `pkg_check_modules()` directly (header explains the
  CMake `Requires` bug).
- A CTest logical suite may span multiple translation units; inspect `howdy/cmake/tests/*.cmake` or
  `pam/cmake/tests/*.cmake` before assuming one file owns a suite.
- Cover every install-time path rule in `cmake/install_config.cmake.in` with a case in
  `tests/cmake/install_config_test.cmake` (`native-howdy-install-config`).

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

## Edge Cases

- **Pre-edit failure matrix:** Before writing or fixing code, enumerate the complete failure space
  across boundary inputs (empty, root/separator/trailing-slash, `.`/`..`, embedded `\0`, symlink or
  non-regular leaves, numeric/size limits), state transitions, syscall/TOCTOU races, and
  rollback/error paths, and audit every sibling entry point sharing that boundary in one pass.
- **Parse once at the boundary:** Parse, validate, and normalize raw primitives
  (`std::filesystem::path`, raw `int` fds, unvalidated strings) in one boundary parser/constructor
  that yields a validated type or owned handle, and pass only that representation across internal
  helpers (keep raw primitives at outer API or thin single-syscall edges). Call normalization and
  path splitting (`lexically_normal()`, `parent_path()`) in that single boundary function only.
  Comments asserting an earlier check made a helper safe (e.g., *"safe now that X ran"*) signal a
  broken boundary—encode the invariant in the validated type instead.
- **Invariant fix and regression lock:** On every bug or review finding, identify the broken system
  invariant before editing, repair the shared boundary parser/helper and all sibling callers
  uniformly rather than stacking localized symptom guards, and lock the fix with table-driven
  negative tests covering the entire malformed input or failure class across every affected entry
  point—not just the single reported case.
- **Boundary ownership:** Fix an edge case in code only when the fix closes the gap at the boundary
  that owns the invariant. When closing it in code would require userspace to re-derive kernel
  semantics (ptrace, credentials, signals, job control) or would only shift assumptions onto another
  caller, do not add a userspace workaround: document the known limitation and owning invariant
  inline at the owning boundary, enforce the contract there, and treat documented limitations as
  settled contracts during review unless the owning boundary changes.

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
