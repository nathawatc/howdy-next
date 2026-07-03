# Repository Guidelines

**Updated:** 2026-07-03

## Scope

Root rules. Read nearest `AGENTS.md` for overrides.

- `ci/AGENTS.md`: CI build/test configuration
- `howdy/src/AGENTS.md`: shared runtime, storage, config, model, helper code
- `howdy/src/cli/AGENTS.md`: CLI entrypoints and download/config commands
- `howdy/src/recorders/AGENTS.md`: camera capture layer
- `pam/AGENTS.md`: PAM module and auth flow

## Build, Test, Development

Use Meson + Ninja; no CMake.

```bash
meson setup build
ninja -C build
meson test -C build --print-errorlogs
```

`meson setup build` configures build dir. `ninja -C build` builds unified
`howdy`, native helpers, compare process, and PAM module. `meson test` runs
native suite.

- Install user-facing commands through `howdy`; no standalone executables.
- Keep privileged helpers under `<libexecdir>/howdy`; do not expose as normal
  commands. This includes `howdy-compare` and setuid `howdy-auth-helper`.

CI runs in container (`ci/Containerfile`); image at
`codeberg.org/nathawat/howdy-next:ci-1`.

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
- Follow `.clang-format`.
- Keep snake_case for files, functions, and tests.
- Preserve tabs in touched C/C++ files.
- Reuse shared helpers for storage, config, readiness, model checks.
- Runtime code should load typed `RuntimeConfig` via `load_runtime_config()`,
  not raw `ConfigReader` lookups.
- Keep changes small and local.

Format C/C++ changes with:

```bash
find howdy pam -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.c' -o -name '*.h' \) -print0 | xargs -0 clang-format -i
```

CI does not run `clang-tidy`; run it before submit:

```bash
run-clang-tidy -p build -quiet
```

## Testing

Tests live in `howdy/src/tests/` and `pam/src/tests/`. Add focused `*_test.cpp`
beside changed code.

- Unified CLI dispatch: `howdy_dispatch_test.cpp`
- Add CLI: `add_cli_test.cpp`, `enrollment_capture_test.cpp`
- List / remove / set CLI: `list_cli_test.cpp`, `remove_cli_test.cpp`,
  `set_cli_test.cpp`
- Snapshot CLI: `snapshot_cli_test.cpp`, `snapshot_writer_test.cpp`
- Test CLI: `test_cli_test.cpp`
- Face detection parsing: `face_detection_test.cpp`
- Atomic file lifecycle: `download_models_test.cpp`
- User model codec: `user_model_codec_test.cpp`
- Compare runtime / frames: `compare_engine_test.cpp`,
  `compare_capture_session_test.cpp`, `compare_logic_test.cpp`,
  `frame_processing_test.cpp`, `face_matching_test.cpp`
- PAM runtime / prompting: `auth_helper_test.cpp`,
  `auth_flow_helpers_test.cpp`, `runtime_session_test.cpp`,
  `prompt_coordinator_test.cpp`
- Config / runtime / storage: `config_*_test.cpp`, `runtime_*_test.cpp`,
  `user_models_test.cpp`

Unified dispatcher and install-layout coverage:
`native-howdy-dispatch`, `native-howdy-install-layout`.

For security-sensitive code, cover failure and success paths. Watch file
ownership checks, config validation, typed runtime config loading, PAM status
mapping, runtime staging, and exception handling.

Run all tests with `meson test -C build --print-errorlogs`. For one test, use
`meson test -C build <test-name> --print-errorlogs`.

## Security

Do not change `config.ini` format casually. Preserve atomic config rewrites,
secure path validation, typed runtime config validation, and ownership
expectations for `/etc/howdy`, `config.ini`, user model files, and custom model
paths. PAM auth should fail closed on unexpected errors; validate helper output
protocol via shared keys in `common/auth_helper_protocol.hpp`.

## Commit / PR

Recent history uses Conventional Commits, for example `fix(pam): ...`,
`test(config): ...`, `style(format): ...`, `build(release): ...`,
`refactor(cli): ...`, `ci: ...`. Keep subjects imperative and scoped.

PRs should include problem statement, concise change summary, linked issues
when applicable, and test results. Include screenshots or terminal output only
when they clarify CLI, PAM prompt, or packaging behavior.
