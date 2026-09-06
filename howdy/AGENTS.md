# Howdy Package Guidelines

**Updated:** 2026-08-15

## Scope

Covers `howdy/include/`, `howdy/src/`, `howdy/tests/`, and package resources
under `howdy/`.

For CLI work across mirrored include/source/test paths, also read
`howdy/src/cli/AGENTS.md`. For vision work, also read
`howdy/src/vision/AGENTS.md`.

## Architecture and Conventions

- Runtime consumers use typed `RuntimeConfig`, `VideoConfig`, and `FaceConfig`;
  do not add direct `ConfigReader` lookups. `RuntimeConfigLoadResult::config`
  is optional; check status and `config.has_value()` before dereferencing.
- `config/config_schema.*` is the single source of truth for runtime option
  metadata, validation rules, and packaged fallback values. Generate packaged
  `config.ini` from it; do not edit generated config manually. Keep runtime-only
  defaults in `src/config/runtime_config_defaults.cpp`.
- Use `support/fd_io.hpp` functions such as
  `read_fd_to_string_bounded()` and `write_all_to_fd()` for bounded FD I/O.
- Use `vision/frame_validation.hpp` before frame transformations or inference.
  Validate dimensions, channels, and element type at the boundary.
- `vision/face_detection.hpp` owns the detector-output parsing boundary.
  `FaceModel::detect()` returns semantic `FaceDetectionResult`; distinguish
  valid zero detections from `kInferenceError` and `kInvalidOutput`. Do not pass
  raw YuNet rows outside face-model internals.
- `storage/user_model_codec.*` owns user-model JSON parsing and serialization.
  Preserve strict grammar, duplicate direct-key rejection, nesting limits,
  numeric bounds, finite-encoding validation, and mutations through
  `user_model_codec::Document`.
- `storage/user_model_store.cpp` owns secure user-model storage transactions:
  path/ownership checks, locking, snapshot consistency, codec integration,
  staged writes/removal, cleanup, and durable completion. Do not duplicate
  storage transactions in callers.
- Reuse `support/atomic_files.hpp` for staged files, metadata handling, fsync,
  rename, cleanup, and parent-directory sync. Treat commit-sync failures as
  potentially committed and report them without silently retrying destructive
  work.
- Use shared model readiness/integrity checks from
  `model_assets/model_file.hpp` and user-model readiness code before inference
  or authentication paths. User-model readiness is organized across:
  - `src/storage/user_model_readiness.cpp`: high-level readiness routing and
    canonical model readiness/security checks
  - `src/storage/user_model_readiness/staged.cpp`: auth-helper staged-runtime
    path, ACL, directory, visible/backing model validation
  - `src/storage/user_model_readiness/internal.hpp`: private cross-TU
    staged-readiness contract (internal plumbing only, not installed, not public)
- Auth-helper protocol keys are shared through
  `protocol/auth_helper_protocol.hpp`; validate both required keys and reject
  malformed, duplicate, unknown, or incomplete output.
- `storage/user_model_store_test_hooks.hpp` remains under production includes
  because `user_model_store.cpp` implements always-linked deterministic hooks.
  Do not expose package test include directories to production targets.
- Keep OpenCV include directories marked as system includes through
  `howdy_opencv`.

## Production CLI Boundaries

Production CLI integration adapters may be separated from injected/shared
command policy when an existing dependency boundary already permits separation.
Do not create a split by pattern alone, and do not imply every command needs a
`*_main.cpp` file.

- `src/cli/add.cpp` owns command policy and
  `add_main_with_dependencies()`:
  argument/label policy, status/diagnostic mapping, and injected orchestration.
- `src/cli/add_main.cpp` owns production `RuntimeConfig` loading,
  `FaceModel` lifecycle, existing-model inspection,
  `VideoCapture`/enrollment integration, storage adapters, and production
  `add_main()`.
- `src/cli/snapshot.cpp` owns frame validation, secure snapshot-directory
  handling, composition/writing, staged atomic installation, and
  `snapshot_main_with_dependencies()`.
- `src/cli/snapshot_main.cpp` owns production camera capture, timestamp/path
  generation, `cv::imencode`, runtime adapters, and production `snapshot_main()`.

## Compare Runtime Boundaries

- `compare/capture_session.*` owns `VideoCapture` lifecycle, open/read results,
  timeout clock, frame numbering, black/dark/valid-frame statistics, and
  exposure restoration.
- `compare/engine.*` owns grayscale preprocessing, CLAHE, brightness and
  black-frame classification, resize/rotation, prepared-frame validation, face
  detection/embedding, and first accepted match selection.
- `src/bin/compare.cpp` remains composition: arguments, runtime config and
  user-model loading, sandbox, `FaceModel` adapters, session/engine
  orchestration, timeout/output/report policy, exit mapping, and the outer
  exception boundary. Keep user-visible output and policy decisions there.
- Compare capture tests use injected capture/clock callbacks; compare engine
  tests use injected inference callbacks and stay ONNX-free.
- Compare privilege dropping is organized as one security-sensitive subsystem
  split across:
  - `src/compare/privileges.cpp`: high-level privilege-drop policy/orchestration
  - `src/compare/privileges/operations.cpp`: production syscall adapters and
    account lookup
  - `src/compare/privileges/verification.cpp`: credential/capability transition
    and verification
  - `src/compare/privileges/internal.hpp`: private cross-TU implementation
    contract (internal plumbing only, not installed, not public)
  - `include/compare/privileges_internal.hpp`: existing test/dependency-injection
    contract, distinct from the private production header

## Production Cohesion

Large cohesive production components should remain intact. File size alone is
not a reason to split them. These boundaries are intentional unless a proven
maintenance problem requires change:

- `storage/user_model_store.cpp`
- `storage/user_model_codec.cpp`
- `config/config_schema.cpp`
- `config/config_utils.cpp`
- `auth_helper/runtime.cpp`
- `app/command_catalog.cpp`
- `cli/download_models.cpp`
- Compare privilege dropping (`src/compare/privileges.cpp`,
  `src/compare/privileges/operations.cpp`,
  `src/compare/privileges/verification.cpp`, and
  `src/compare/privileges/internal.hpp`): treat these files as one cohesive,
  security-sensitive subsystem. Do not fragment the subsystem further merely
  because individual files become long. Explicitly preserve privilege
  transition ordering, identity ownership assumptions, capability clearing,
  root-regain checks, and fatal fail-closed behavior.
- User-model readiness (`src/storage/user_model_readiness.cpp`,
  `src/storage/user_model_readiness/staged.cpp`, and
  `src/storage/user_model_readiness/internal.hpp`): treat these files as one
  cohesive, security-sensitive subsystem. Do not fragment the subsystem
  further merely for LOC. Explicitly preserve the invariant that staged runtime
  validation remains descriptor-relative and fail-closed.

## Tests

CTest logical suites may span multiple `.cpp` files. A driver or file owning
`main()` is not the complete suite. Inspect `howdy/CMakeLists.txt` and sibling
`*_test.cpp` files before moving coverage or adding helpers. Headers shared only
by test translation units belong under `tests/include/<module>/`.

Representative current decompositions:

- User-model codec: `user_model_codec_test.cpp`,
  `user_model_codec_parsing_test.cpp`, `user_model_codec_document_test.cpp`,
  `user_model_codec_limits_test.cpp`.
- User-model storage: `user_models_test.cpp`,
  `user_models_mutation_test.cpp`, and `user_models_failure_test.cpp`.
- Config utilities: `config_utils_test.cpp`, `config_read_update_test.cpp`,
  `config_atomic_write_test.cpp`, `config_atomic_replace_test.cpp`, and
  `config_path_security_test.cpp`.
- Compare engine: `compare_engine_test.cpp`,
  `compare_engine_frame_test.cpp`, and `compare_engine_inference_test.cpp`.
- Compare privileges: `compare_privileges_test.cpp`,
  `compare_privileges_non_root_test.cpp`,
  `compare_privileges_privileged_test.cpp`, and
  `compare_privileges_fatal_test.cpp`.
- Add CLI, config CLI, snapshot/preview, download-models, dispatcher/completion,
  and auth-helper tests are also logical suites; use CMake source lists rather
  than assuming one source owns all behavior.
