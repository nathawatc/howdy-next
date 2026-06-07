# Repository Guidelines

**Updated:** 2026-06-04  
**Base commit:** b06c8dd

## Project Structure & Module Organization

This repository builds Howdy Next, a Linux facial-recognition authentication stack with a native C++ CLI/runtime and PAM module.

- `howdy/src/`: CLI entrypoints, compare runtime, config, storage, recorders, core face model code, and unit tests in `howdy/src/tests/`.
- `howdy/include/`: shared headers for CLI, config, common helpers, storage, recorders, and model code.
- `pam/`: PAM module sources, headers, translations, man page, and tests in `pam/src/tests/`.
- `config/config.ini`: packaged default configuration template.
- `archlinux/`: Arch Linux packaging files for release and `-git` packages.
- `subprojects/`: Meson wraps, including `inih`.

## Build, Test, and Development Commands

Use Meson and Ninja; this project does not use CMake.

```bash
meson setup build
ninja -C build
meson test -C build --print-errorlogs
```

`meson setup build` configures the build directory, `ninja -C build` compiles the CLI, compare binary, and PAM module, and `meson test` runs the native test suite with failure logs.

Useful local commands after install include:

```bash
howdy add <user>
howdy test
howdy list
howdy disable
howdy download-models
```

## Coding Style & Naming Conventions

C++ code is formatted with the repository `.clang-format` policy. Use snake_case for files, functions, and test names, matching existing files such as `compare_logic.cpp` and `config_reader_test.cpp`. Keep shared helpers in `howdy/include/common/` or `howdy/include/config/` when behavior crosses modules.

Format C/C++ changes with:

```bash
find howdy pam -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.c' -o -name '*.h' \) -print0 | xargs -0 clang-format -i
```

Run static analysis when practical:

```bash
run-clang-tidy -p build -quiet
```

## Testing Guidelines

Tests are Meson-registered native executables under `howdy/src/tests/` and `pam/src/tests/`. Add focused tests beside the module being changed, using the `*_test.cpp` naming pattern. For security-sensitive code, cover failure paths as well as success paths, especially file ownership checks, config validation, PAM status mapping, and exception handling.

Run all tests with `meson test -C build --print-errorlogs`. For a single test, use `meson test -C build <test-name> --print-errorlogs`.

## Commit & Pull Request Guidelines

Recent history uses Conventional Commits, for example `fix(pam): ...`, `test(config): ...`, `style(format): ...`, and `build(release): ...`. Keep subjects imperative and scoped.

Pull requests should include a clear problem statement, a concise change summary, linked issues when applicable, and test results. Include screenshots or terminal output only when they clarify CLI, PAM prompt, or packaging behavior.

## Security & Configuration Tips

Do not change the `config.ini` format casually. Preserve atomic config rewrites, secure path validation, and ownership expectations for `/etc/howdy`, `config.ini`, user model files, and custom model paths. PAM authentication should fail closed on unexpected errors.
