# Howdy Source Guidelines

**Updated:** 2026-07-05

## Scope

Shared runtime code under `howdy/src/` and headers under `howdy/include/`.

## Conventions

- Prefer shared helpers for storage, config, readiness, model checks, and
  runtime staging.
- Do not reintroduce duplicated file-validation or config-loading logic in
  feature code.
- Runtime consumers should use typed `RuntimeConfig`, `VideoConfig`, and
  `FaceConfig` values instead of direct `ConfigReader` key lookups.
- `RuntimeConfigLoadResult::config` is optional; check status and
  `config.has_value()` before dereferencing.
- Keep runtime behavior fail-closed on validation or ownership errors.
- Use C++23 idioms when editing shared runtime code.
- Use centralized `read_fd_to_string_bounded()` and `write_fd_from_buffer()`
  from `common/fd_io.hpp` for bounded FD I/O.
- Use `common/frame_validation.hpp` to validate OpenCV frame shape,
  dimensions, channels, and element type before transformations or inference.
- Use `core/face_matching.hpp` for pure embedding candidate selection; do not
  duplicate metric or finite-value logic.
- Use `core/face_detection.hpp` as detector-output boundary.
  `FaceModel::detect()` returns `FaceDetectionResult`; distinguish valid zero
  detections from `kInferenceError` and `kInvalidOutput`.
- Consume semantic `FaceDetection` fields (`box`, `landmarks`, `confidence`);
  do not pass raw YuNet `cv::Mat` rows outside face-model internals or
  reimplement detector-row parsing.
- User-model JSON parsing and serialization live in `storage/user_model_codec.*`
  and use yyjson. Preserve strict parsing, duplicate direct-key rejection,
  nesting limits, numeric bounds, and finite-encoding validation.
- Keep codec mutations through `user_model_codec::Document`; do not add ad hoc
  JSON parsing or serialization in storage callers.
- Reuse `common/atomic_files.hpp` staged-file helpers for atomic writes. Do not
  duplicate temporary file creation, metadata preservation, fsync, rename,
  cleanup, or parent-directory sync logic.
- Use `face_detection_test.cpp` for pure YuNet-result parsing and
  `user_model_codec_test.cpp` for codec grammar/security regressions; neither
  requires real ONNX inference.
- Enrollment capture logic lives in `cli/enrollment_capture.hpp` (template
  `capture_enrollment_sample()`); classify failures with
  `classify_enrollment_capture_failure()`.
- Auth helper protocol keys are shared via `common/auth_helper_protocol.hpp`.
- Build requires `opencv4` with `include_type: 'system'` for system OpenCV.

## Compare Runtime Boundaries

- `common/compare_capture_session.hpp` owns `VideoCapture` lifecycle, open/read
  results, timeout clock, frame numbering, black/dark/valid frame stats, and
  exposure restore.
- `common/compare_engine.hpp` owns grayscale preprocessing, CLAHE, brightness
  classification, black-frame filtering, resize/rotation, prepared-frame
  validation, face detection, embedding, and first accepted match selection.
- `compare.cpp` stays composition layer: args, runtime config and user-model
  load, sandbox, `FaceModel` adapters, session/engine orchestration,
  timeout/output/report policy, `CompareExit` mapping, and outer exception
  boundary.
- Do not move user-visible output, PAM-facing exit mapping, or policy
  decisions into helpers.
- `compare_capture_session_test.cpp` stays camera-free through injected
  capture/clock callbacks.
- `compare_engine_test.cpp` stays ONNX-free through injected inference
  callbacks.
