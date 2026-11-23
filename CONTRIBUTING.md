# Contributing to Howdy Next

Thanks for contributing to Howdy Next.

Howdy Next is a Linux facial-recognition auth stack with a native C++ CLI/runtime and PAM module.
Keep changes secure, maintainable, and aligned with the repository guidelines.

## Start Here

Read the repository guidelines before opening an issue or pull request:

- [Repository Guidelines](AGENTS.md)
- [Code of Conduct](CODE_OF_CONDUCT.md)

[`AGENTS.md`](AGENTS.md) is the single source of truth for repository-wide invariants, layout,
security rules, and commit conventions. Read the nearest nested `AGENTS.md` for subsystem-specific
guidance.

## Licensing

By submitting a contribution to Howdy Next, you agree to license your contribution under the GNU
General Public License v3.0 or later (`GPL-3.0-or-later`).

## Development Workflow

Use CMake 3.31+ with GCC/Clang and GNU Make/Ninja (see
[Build and CMake](AGENTS.md#build-and-cmake)). Installing `ccache` is recommended for faster
incremental rebuilds; Howdy uses it automatically when available (pass `-DHOWDY_USE_CCACHE=OFF` at
configure time to disable it).

- **Build, CTest (`release`/`debug`), and incremental `clang-tidy`**: follow
  [`verify-changes`](.agents/skills/verify-changes/SKILL.md).
- **Formatting and linting (C/C++, CMake, Markdown, shell)**: follow
  [`format-code`](.agents/skills/format-code/SKILL.md) (see also
  [Code and Layout](AGENTS.md#code-and-layout)).
- **Local containerized CI (Arch Linux and Nix)**: follow
  [`run-local-ci`](.agents/skills/run-local-ci/SKILL.md).
- **PAM translations (`howdy-pot`, `howdy-update-po`, `codegen`)**: follow
  [`update-translations`](.agents/skills/update-translations/SKILL.md).
- **Privileged installed PAM/setuid E2E (`run0`)**: follow
  [`run-privileged-e2e`](.agents/skills/run-privileged-e2e/SKILL.md).
- **Protocol libFuzzer targets (Clang)**: follow
  [`run-fuzzers`](.agents/skills/run-fuzzers/SKILL.md).

### Cross-Compilation

Cross-compiling requires a suitable target toolchain or sysroot and
`CMAKE_CROSSCOMPILING_EMULATOR`. Howdy builds target-side tools (`howdy_config_generator`,
`howdy_docs_generator`, and staged preflight checks) and executes them during the build and install
steps.

Target dependencies (OpenCV, PAM, libcurl, etc.) must be provided by the target sysroot or
toolchain; Howdy does not provide or build target dependencies.

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

## Pull Requests

Include:

- clear problem statement
- concise change summary
- linked issues when useful
- test results
- logs, screenshots, or terminal output when they clarify behavior

Follow the commit message format (Conventional Commits, 72-character subject cap) in
[Commits](AGENTS.md#commits).

## Security-Sensitive Changes

Howdy Next is authentication software. Changes touching PAM behavior, config parsing, model
storage, path validation, ownership checks, or `/etc/howdy` handling require strict review; follow
the standing invariants in [Security](AGENTS.md#security) and the nearest subsystem `AGENTS.md`.

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

Feature requests should describe the use case, expected behavior, and why the change belongs in
Howdy Next rather than local PAM, packaging, or desktop configuration.
