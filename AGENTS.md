# Repository Guidelines

**Updated:** 2026-07-16

## Scope

Root rules. Read nearest `AGENTS.md` for overrides.

- `ci/AGENTS.md`: CI build/test configuration
- `howdy/AGENTS.md`: Howdy package include, source, tests, and resources
- `howdy/src/cli/AGENTS.md`: CLI entrypoints and download/config commands
- `howdy/src/vision/AGENTS.md`: OpenCV 5 DNN and camera capture layer
- `pam/AGENTS.md`: PAM module and auth flow

## Build, Test, Development

Use CMake 3.31+ with GCC/Clang and GNU Make/Ninja.

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

CMake configures build dir. Build produces unified `howdy`, native helpers, compare process, PAM module.
CTest runs native suite.

- Install user-facing commands through `howdy`; no standalone executables.
- Keep privileged helpers under `<libexecdir>/howdy`; do not expose as normal
  commands. This includes `howdy-compare` and setuid `howdy-auth-helper`.

CI runs in container (`ci/Containerfile`); image at
`codeberg.org/nathawat/howdy-next/ci-1:latest`.

Useful local commands post-install:

```sh
howdy add <user>
howdy test
howdy list
howdy disable
howdy download-models
```

## Code Style

- Target C++23.
- Follow `.clang-format`.
- snake_case for files, functions, tests.
- Preserve tabs in touched C/C++ files.
- Keep the Cargo-style layout: mirror modules across `include/`, `src/`, and `tests/` when applicable;
  keep resources outside `src/`.
- Reuse shared helpers for storage, config, readiness, model checks.
- Runtime code should load typed `RuntimeConfig` via `load_runtime_config()`,
  not raw `ConfigReader` lookups.
- Keep changes small and local.

Format C/C++:

```sh
find howdy pam -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.c' -o -name '*.h' \) -print0 | xargs -0 clang-format -i
```

CI does not run `clang-tidy`; do not use `run-clang-tidy`; use `clang-tidy-diff.py` only:

```sh
git diff -U0 HEAD -- howdy pam | /usr/share/clang/clang-tidy-diff.py -p1 -path build -quiet -hide-progress
```

## Testing

Tests in `howdy/tests/` and `pam/tests/`. Add focused `*_test.cpp` under matching module.

Current test files of interest:

- Dispatcher / install layout: `howdy_dispatch_test.cpp`, `install_layout_test.sh`
- CLI: `add_cli_test.cpp`, `clear_cli_test.cpp`, `config_cli_test.cpp`,
  `disable_cli_test.cpp`, `download_models_test.cpp`, `enrollment_capture_test.cpp`,
  `list_cli_test.cpp`, `remove_cli_test.cpp`, `set_cli_test.cpp`,
  `snapshot_cli_test.cpp`, `test_cli_test.cpp`
- Compare / capture: `compare_args_test.cpp`, `compare_capture_session_test.cpp`,
  `compare_engine_test.cpp`, `compare_logic_test.cpp`, `face_detection_test.cpp`,
  `face_encoding_test.cpp`, `face_matching_test.cpp`, `face_model_test.cpp`,
  `frame_processing_test.cpp`, `preview_engine_test.cpp`,
  `test_preview_session_test.cpp`, `video_capture_test.cpp`
- Config / storage / utilities: `capture_device_path_test.cpp`,
  `config_reader_test.cpp`, `config_utils_test.cpp`, `config_validation_test.cpp`,
  `fd_io_test.cpp`, `file_security_test.cpp`, `invoking_user_env_test.cpp`,
  `model_file_test.cpp`, `runtime_config_load_test.cpp`, `runtime_config_test.cpp`,
  `runtime_paths_test.cpp`, `user_model_codec_test.cpp`, `user_models_test.cpp`,
  `user_names_test.cpp`
- PAM / auth flow: `auth_flow_helpers_test.cpp`, `auth_helper_test.cpp`,
  `main_entrypoints_test.cpp`, `native_prompt_conversation_test.cpp`,
  `optional_task_test.cpp`, `prompt_coordinator_test.cpp`,
  `prompt_workaround_test.cpp`, `runtime_session_test.cpp`,
  `status_mapping_test.cpp`

Unified dispatcher and install-layout coverage:
`native-howdy-dispatch`, `native-howdy-install-layout`.

For security-sensitive code, cover failure and success paths. Watch file
ownership checks, config validation, typed runtime config loading, PAM status
mapping, runtime staging, and exception handling.

Run all tests with `ctest --test-dir build --output-on-failure`. For one test,
use `ctest --test-dir build -R '^<test-name>$' --output-on-failure`.

## Security

Do not change `config.ini` format casually. Preserve atomic config rewrites,
secure path validation, typed runtime config validation, and ownership
expectations for `/etc/howdy`, `config.ini`, downloaded ONNX model files, and
user model files. PAM auth should fail closed on unexpected errors; validate
helper output protocol via shared keys in `protocol/auth_helper_protocol.hpp`.

## Commit / PR

Recent history uses Conventional Commits, for example `fix(pam): ...`,
`test(config): ...`, `style(format): ...`, `build(release): ...`,
`refactor(cli): ...`, `ci: ...`. Keep subjects imperative and scoped.

PRs should include problem statement, concise change summary, linked issues
when applicable, and test results. Include screenshots or terminal output only
when they clarify CLI, PAM prompt, or packaging behavior.
