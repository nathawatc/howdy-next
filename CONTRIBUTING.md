# Contributing to Howdy Next

Thanks for contributing to Howdy Next.

Howdy Next is a Linux facial-recognition auth stack with native C++
CLI/runtime and PAM module. Keep changes secure, maintainable, and aligned with
current build and test flow.

## Start Here

Read repo guidelines before issue or PR:

- [Repository Guidelines](AGENTS.md)
- [Code of Conduct](CODE_OF_CONDUCT.md)

`AGENTS.md` defines repository-wide rules. Read the nearest subsystem
`AGENTS.md` for local ownership, security, and test guidance.

## Licensing

By submitting a contribution to Howdy Next, you agree to license your
contribution under the GNU General Public License v3.0 or later
(`GPL-3.0-or-later`).

## Basic Workflow

Use CMake 3.31+ with GCC/Clang and GNU Make/Ninja. Installing `ccache` is
recommended for faster incremental rebuilds. Howdy uses it automatically when
available; pass `-DHOWDY_USE_CCACHE=OFF` at configure time to disable it.

### Release

```sh
cmake --preset release
cmake --build --preset release --parallel "$(nproc)"
ctest --preset release
```

### Debug

```sh
cmake --preset debug
cmake --build --preset debug --parallel "$(nproc)"
ctest --preset debug
```

## Cross-Compilation

Cross-compiling requires `CMAKE_CROSSCOMPILING_EMULATOR`: Howdy builds target-side
`howdy_config_generator` and `howdy_docs_generator`, then executes them during the
build to generate configuration and documentation.

With a toolchain file:

```sh
cmake -S . -B build/cross \
    -DCMAKE_TOOLCHAIN_FILE=/path/to/toolchain.cmake \
    -DCMAKE_CROSSCOMPILING_EMULATOR=/usr/bin/qemu-aarch64
cmake --build build/cross --parallel "$(nproc)"
```

Pass emulator arguments as a semicolon-separated CMake list when needed:

```sh
-DCMAKE_CROSSCOMPILING_EMULATOR='/usr/bin/qemu-aarch64;-L;/usr/aarch64-linux-gnu'
```

### Installed PAM/setuid End-to-End Test

Privileged installed-path coverage stays opt-in. Build as normal user with dedicated
compile-time prefix, then run only test through `run0`:

```sh
cmake --preset e2e
cmake --build --preset e2e --parallel "$(nproc)"
run0 --setenv=HOWDY_E2E_USER="$USER" ctest --preset e2e
```

Set `HOWDY_E2E_USER` to existing non-root account. Test also accepts valid non-root `SUDO_USER`
supplied by privilege wrappers. Test installs temporarily below configured `/opt` prefix and removes
installation and runtime staging on exit.

### Translation Code Generation

```sh
cmake --build --preset release --target codegen --parallel "$(nproc)"
```

Translation maintenance:

```sh
cmake --build --preset release --target howdy-pot --parallel "$(nproc)"
cmake --build --preset release --target howdy-update-po --parallel "$(nproc)"
```

Format C/C++ with `.clang-format`:

```sh
find howdy pam -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.c' -o -name '*.h' \) -print0 | xargs -0 clang-format -i
```

Static analysis before submit:

```sh
git diff -U0 HEAD -- howdy pam | /usr/share/clang/clang-tidy-diff.py -p1 -path build -quiet -hide-progress
```

## Pull Requests

Include:

- clear problem statement
- concise change summary
- linked issues when useful
- test results
- logs, screenshots, or terminal output when they clarify behavior

Conventional Commits:

```text
fix(pam): handle helper setup failure
perf(compare): reduce frame resize overhead
test(config): cover invalid float values
docs(wiki): update PAM integration examples
build(release): bump version to X.Y.Z
style(format): apply clang-format
```

## Security-Sensitive Changes

Howdy Next is auth software. Changes to PAM behavior, config parsing, model
storage, path validation, ownership checks, or `/etc/howdy` handling need
extra care.

Auth code should fail closed on unexpected errors. Do not bypass permission,
ownership, symlink, hardlink, or writable trust-root checks.

See [Repository Guidelines](AGENTS.md) before security-sensitive changes.

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
