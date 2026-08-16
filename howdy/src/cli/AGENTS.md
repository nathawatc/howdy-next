# CLI Knowledge Base

**Updated:** 2026-08-15

## Scope

Native C++ CLI for model management, config editing, camera preview, and
snapshot generation. Keep `howdy` as the sole installed user-facing command.

## Where to Look

- Unified dispatch: `howdy/src/app/howdy.cpp` and
  `howdy/src/bin/howdy_main.cpp` — parse global flags, resolve target user,
  enforce root/user guards, and dispatch commands.
- Add policy: `howdy/src/cli/add.cpp` — argument/label policy,
  status/diagnostic mapping, and `add_main_with_dependencies()`.
- Add production adapters: `howdy/src/cli/add_main.cpp` — production
  `RuntimeConfig` loading, `FaceModel` lifecycle, existing-model inspection,
  `VideoCapture`/enrollment adapter, storage adapters, and production
  `add_main()`.
- Clear all: `howdy/src/cli/clear.cpp` — inspect and delete all user models.
- Edit config: `howdy/src/cli/config.cpp` and
  `howdy/src/cli/config_edit_session.cpp` — safe editor workflow and atomic
  installation.
- Toggle auth: `howdy/src/cli/disable.cpp` — enable or disable auth.
- List models: `howdy/src/cli/list.cpp` — show user-model IDs.
- Remove one: `howdy/src/cli/remove.cpp` — delete one model by ID.
- Set config: `howdy/src/cli/set.cpp` — update one config value atomically.
- Download ONNX: `howdy/src/cli/download_models.cpp` — pinned packaged-model
  fetch, integrity, and installation policy.
- Snapshot policy/writer: `howdy/src/cli/snapshot.cpp` — frame validation,
  secure snapshot directory, composition/writer, staged atomic installation,
  and `snapshot_main_with_dependencies()`.
- Snapshot production adapters: `howdy/src/cli/snapshot_main.cpp` — production
  camera capture, timestamp/path generation, `cv::imencode`, runtime adapters,
  and production `snapshot_main()`.
- Camera test: `howdy/src/cli/test.cpp` — one CLI/composition source for
  production preview setup and `test_main_with_dependencies()`/`test_main()`.
  Reusable preview responsibilities live in `test_preview_session.*`,
  `test_preview_renderer.*`, and `preview_engine.*`; do not invent a second
  production entrypoint source for this command.

## Internal Headers and Injection Seams

CLI commands use dependency injection for deterministic tests.

- `include/app/howdy_internal.hpp`: `HowdyDependencies`,
  `howdy_main_with_dependencies()`
- `include/cli/add_internal.hpp`: `AddDependencies`,
  `add_main_with_dependencies()`
- `include/cli/clear_internal.hpp`: `ClearDependencies`,
  `clear_main_with_dependencies()`
- `include/cli/disable_internal.hpp`: `DisableDependencies`,
  `disable_main_with_dependencies()`
- `include/cli/list_internal.hpp`: `ListDependencies`,
  `list_main_with_dependencies()`
- `include/cli/remove_internal.hpp`: `RemoveDependencies`,
  `remove_main_with_dependencies()`
- `include/cli/set_internal.hpp`: `SetDependencies`,
  `set_main_with_dependencies()`
- `include/cli/config_internal.hpp`: `ConfigDependencies`, `TempConfigCopy`,
  `config_main_with_dependencies()`
- `include/cli/test_cli_internal.hpp`: `TestDependencies`,
  `test_main_with_dependencies()`, `run_preview_preflight()`, and
  `has_graphical_display_environment()`
- `include/cli/snapshot_internal.hpp`: `SnapshotDependencies`,
  `SnapshotWriterDependencies`, `snapshot_main_with_dependencies()`,
  `write_snapshot_at_path()`, and `write_snapshot_with_unique_path()`
- `include/cli/enrollment_capture.hpp`: `capture_enrollment_sample()`,
  `EnrollmentCaptureResult`, and `classify_enrollment_capture_failure()`
- `include/cli/download_models_internal.hpp`: download-model internals

`add.cpp` and `snapshot.cpp` contain shared/injected command policy. Their
`*_main.cpp` files contain production integration only because those existing
boundaries reduce dependency coupling. This is not a requirement for every CLI
command.

`snapshot_internal::kSnapshotFrameCount` is the single frame-count invariant
shared by injected snapshot policy and production camera capture.

## Conventions

- Add commands through dispatcher dependencies in
  `include/app/howdy_internal.hpp`; do not add standalone executables.
- Dispatcher owns top-level syntax, `--` end-of-options handling, command
  metadata validation, `-U/--user` target resolution, root requirement, root-user
  rejection, and invalid model-user validation. It forwards applicable normalized
  `--plain`/`-y` flags and injects resolved model users. Subcommand parsers own
  strict validation of that normalized argv and command-specific options.
- Keep config edits secure and atomic. Reuse `support/invoking_user*.hpp`,
  `model_assets/model_file.hpp`, config helpers, storage helpers, and runtime
  readiness checks.
- Runtime commands use typed `RuntimeConfig` fields rather than raw
  `ConfigReader` access.
- `download-models` keeps pinned upstream URLs, SHA-256 values, and the
  canonical OpenCV 5 model pair. Missing required injected callbacks must fail
  closed before filesystem or network work.
- Snapshot writer validates BGR frame batches and atomically installs output.
  If directory sync fails after installation, report that the file may already
  exist; do not silently retry destructive work.
- Enrollment diagnostics use `classify_enrollment_capture_failure()` for black,
  dark, unusable, and no-face outcomes.
- Preserve config command cleanup and stale-content semantics: invalid edited
  content keeps its temporary file; other terminal paths remove it exactly once;
  stale detection passes original content as `expected_current_content`; install
  keeps `lock = true` and `validate_runtime = false`.
- Keep production editor/environment behavior in production adapters; injected
  tests must not alter `$EDITOR` policy.

## Tests

CTest logical suites may contain several translation units. Inspect
`howdy/CMakeLists.txt` and sibling `*_test.cpp` files; do not treat a driver as
the complete suite.

- Add CLI: `tests/cli/add_cli_test.cpp` is the driver, with
  `add_cli_preflight_test.cpp`, `add_cli_capture_test.cpp`, and
  `add_cli_arguments_test.cpp`.
- Config CLI: `tests/cli/config_cli_test.cpp` is the driver, with
  `config_cli_workflow_test.cpp` and `config_cli_integration_test.cpp`.
- Download models: `tests/cli/download_models_test.cpp` participates in the
  multi-source `native-download-models` suite with focused entrypoint,
  integrity, manifest, and support sources.
- Preview: `tests/cli/test_preview_session_test.cpp` and
  `tests/cli/test_preview_renderer_test.cpp` form the preview session suite.
- Snapshot: `tests/cli/snapshot_cli_test.cpp` and
  `tests/cli/snapshot_writer_test.cpp` are separate CLI and writer suites.
- Dispatcher/completion belongs under `tests/app/`, including
  `howdy_dispatch_test.cpp` and `howdy_completion_test.cpp`, not under CLI
  source ownership.

Use injected capture, clock, renderer, and inference dependencies for
orchestration tests. Keep test-only shared headers under `tests/include/`.
