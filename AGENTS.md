# Repository Guidelines

**Updated:** 2026-09-14

This file defines repository-wide rules. Read the nearest `AGENTS.md` before changing a subsystem;
local files add detail but do not override these invariants.

## Repository Map

- [`ci/archlinux/AGENTS.md`](ci/archlinux/AGENTS.md): Arch Linux CI image and workflow rules
- [`ci/nix/AGENTS.md`](ci/nix/AGENTS.md): Nix CI image, Flake, and module checks
- [`howdy/AGENTS.md`](howdy/AGENTS.md): native Howdy package
- [`pam/AGENTS.md`](pam/AGENTS.md): PAM module

Package-level files route to narrower subsystem guidance.

## Build and Test

Use CMake 3.31+ and C++23.

```sh
cmake --preset release
cmake --build --preset release --parallel "$(nproc)"
ctest --preset release
```

```sh
cmake --preset debug
cmake --build --preset debug --parallel "$(nproc)"
ctest --preset debug
```

Use focused tests while iterating, then run the full relevant preset before closing a change. A
CTest suite may span several translation units; inspect CMake source lists before assuming one test
file owns the suite.

## Code and Layout

- Follow `.clang-format`; preserve tabs in touched C/C++ files.
- Use snake_case for files, functions, and tests.
- A production component normally exposes one module-level pair: `src/<module>/<component>.cpp` and
  `include/<module>/<component>.hpp`.
- Put subordinate production implementation under `src/<module>/<component>/`.
- Put shared/test-facing subordinate contracts under `include/<module>/<component>/` only when
  production or tests need them.
- Keep private cross-TU contracts under the matching `src/.../<component>/internal.hpp`.
- Do not create empty mirror directories for visual symmetry.
- `src/bin/` and `src/tools/` are entrypoint/tool exceptions.
- Keep test-only shared headers under `tests/include/`; never add test include roots to production
  targets.

For formatting:

```sh
find howdy pam -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.c' -o -name '*.h' \) -print0 | xargs -0 clang-format -i
```

CI does not run `clang-tidy`. For changed code, use `clang-tidy-diff.py` against the configured
build tree.

## Security

Preserve fail-closed behavior, privilege boundaries, ownership/mode checks, descriptor lifetime,
lock ordering, staged-file semantics, and durability handling. Treat a commit-sync failure as
potentially committed. Do not weaken `/etc/howdy`, model, user-model, PAM, or helper protocol checks
to simplify code.

Runtime code uses typed `RuntimeConfig`; do not add raw `ConfigReader` lookups to consumers.

## Commits

Use Conventional Commits with imperative subjects, for example `fix(pam): ...`, `refactor(config):
...`, `test(storage): ...`, or `build(release): ...`. Do not push unless explicitly requested.
