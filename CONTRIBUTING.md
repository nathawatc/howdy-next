# Contributing to Howdy Next

Thanks for contributing to Howdy Next.

Howdy Next is a Linux facial-recognition auth stack with native C++
CLI/runtime and PAM module. Keep changes secure, maintainable, and aligned with
current build and test flow.

## Start Here

Read repo guidelines before issue or PR:

- [Repository Guidelines](AGENTS.md)
- [Code of Conduct](CODE_OF_CONDUCT.md)

`AGENTS.md` is main contributor reference for layout, build commands, test
commands, coding style, commit format, PR expectations, and security notes.

## Basic Workflow

Use Meson and Ninja. No CMake.

```bash
meson setup build
ninja -C build
meson test -C build --print-errorlogs
```

Format C/C++ changes with repo `.clang-format`:

```bash
find howdy pam -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.c' -o -name '*.h' \) -print0 | xargs -0 clang-format -i
```

Run static analysis before submit:

```bash
run-clang-tidy -p build -quiet
```

## Pull Requests

Include:

- clear problem statement
- concise change summary
- linked issues when useful
- test results
- logs, screenshots, or terminal output when they clarify behavior

Use Conventional Commits, for example:

```text
fix(pam): handle helper setup failure
perf(compare): reduce frame resize overhead
test(config): cover invalid float values
docs(wiki): update PAM integration examples
build(release): bump version to 3.2.0
style(format): apply clang-format
```

## Security-Sensitive Changes

Howdy Next is auth software. Changes to PAM behavior, config parsing, model
storage, path validation, ownership checks, or `/etc/howdy` handling need
extra care.

Auth code should fail closed on unexpected errors. Do not bypass permission,
ownership, symlink, hardlink, or writable trust-root checks.

See [Repository Guidelines](AGENTS.md) before changing security-sensitive code.

## Issues

Bug reports should include:

- Howdy Next version
- distribution and package source
- desktop environment or lock screen when relevant
- camera model or device path when relevant
- affected command or PAM service
- exact error output
- relevant `journalctl -b` or auth log lines

Remove private data before posting logs.

Feature requests should describe use case, expected behavior, and why change
belongs in Howdy Next rather than local PAM, packaging, or desktop config.
