# Contributing to Howdy Next

Thank you for contributing to Howdy Next.

Howdy Next is a Linux facial-recognition authentication stack with a native C++
CLI/runtime and PAM module. Contributions should keep the project secure,
maintainable, and compatible with the existing build and test workflow.

## Start Here

Before opening an issue or pull request, read the repository guidelines:

- [Repository Guidelines](AGENTS.md)
- [Code of Conduct](CODE_OF_CONDUCT.md)

`AGENTS.md` is the main contributor reference for project layout, build
commands, test commands, coding style, commit format, pull request expectations,
and security-sensitive development notes.

## Basic Workflow

Use Meson and Ninja. This project does not use CMake.

```bash
meson setup build
ninja -C build
meson test -C build --print-errorlogs
```

Format C/C++ changes with the repository `.clang-format` policy:

```bash
find howdy pam -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.c' -o -name '*.h' \) -print0 | xargs -0 clang-format -i
```

Contributors must run static analysis before submitting changes:

```bash
run-clang-tidy -p build -quiet
```

## Pull Requests

Pull requests should include:

- a clear problem statement
- a concise summary of the change
- linked issues when applicable
- test results
- relevant logs, screenshots, or terminal output when they clarify behavior

Use Conventional Commits, for example:

```text
fix(pam): handle helper setup failure
perf(compare): reduce frame resize overhead
test(config): cover invalid float values
docs(wiki): update PAM integration examples
build(release): prepare 3.1.2
style(format): apply clang-format
```

## Security-Sensitive Changes

Howdy Next is authentication software. Changes to PAM behavior, config parsing, model storage, path
validation, ownership checks, or `/etc/howdy` handling require extra care.

Authentication code should fail closed on unexpected errors. Do not bypass permission, ownership,
symlink, hardlink, or writable trust-root checks.

See [Repository Guidelines](AGENTS.md) before changing security-sensitive code.

## Issues

Bug reports should include:

- Howdy Next version
- distribution and package source
- desktop environment or lock screen when relevant
- camera model or device path when relevant
- affected command or PAM service
- exact error output
- relevant `journalctl -b` or authentication log lines

Remove private data before posting logs.

Feature requests should describe the use case, expected behavior, and why the change belongs in
Howdy Next rather than local PAM, packaging, or desktop configuration.
