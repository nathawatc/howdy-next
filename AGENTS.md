# Repository Guidelines

**Updated:** 2026-06-11

## Scope

Root rules for whole repo. Read nearest `AGENTS.md` for local overrides.

- `howdy/src/AGENTS.md`: shared runtime, storage, config, model, and helper code
- `howdy/src/cli/AGENTS.md`: CLI entrypoints and download/config commands
- `howdy/src/recorders/AGENTS.md`: camera capture layer
- `pam/AGENTS.md`: PAM module and auth flow

## Build, Test, Development

Use Meson and Ninja; this project does not use CMake.

```bash
meson setup build
ninja -C build
meson test -C build --print-errorlogs
```

`meson setup build` configures build dir, `ninja -C build` compiles CLI, compare binary, PAM module, `meson test` runs native suite with failure logs.

Useful local commands after install:

```bash
howdy add <user>
howdy test
howdy list
howdy disable
howdy download-models
```

## Code Style

- Target C++23.
- Follow repository `.clang-format`.
- Keep snake_case for files, functions, and tests.
- Preserve tabs indentation in touched C/C++ files.
- Reuse shared helpers for storage, config, readiness, and model checks.
- Keep changes small and local to module boundaries.

Format C/C++ changes with:

```bash
find howdy pam -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.c' -o -name '*.h' \) -print0 | xargs -0 clang-format -i
```

Run static analysis when practical:

```bash
run-clang-tidy -p build -quiet
```

## Testing

Tests are Meson-registered native executables under `howdy/src/tests/` and `pam/src/tests/`. Add focused tests beside changed code, using `*_test.cpp`.

For security-sensitive code, cover failure paths and success paths. Watch file ownership checks, config validation, PAM status mapping, runtime staging, and exception handling.

Run all tests with `meson test -C build --print-errorlogs`. For a single test, use `meson test -C build <test-name> --print-errorlogs`.

## Security

Do not change `config.ini` format casually. Preserve atomic config rewrites, secure path validation, and ownership expectations for `/etc/howdy`, `config.ini`, user model files, and custom model paths. PAM auth should fail closed on unexpected errors.

## Commit / PR

Recent history uses Conventional Commits, for example `fix(pam): ...`, `test(config): ...`, `style(format): ...`, and `build(release): ...`. Keep subjects imperative and scoped.

PRs should include problem statement, concise change summary, linked issues when applicable, and test results. Include screenshots or terminal output only when they clarify CLI, PAM prompt, or packaging behavior.
