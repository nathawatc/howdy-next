# Vision Guidelines

Parent: [`../../AGENTS.md`](../../AGENTS.md).

## Model Baseline

- Pin the OpenCV 5 New DNN graph engine via `OPENCV_FORCE_DNN_ENGINE` (`ForcedDnnEngine()` in
  `include/vision/face_model/internal.hpp`, handling the OpenCV 5.0 vs 5.1+ rename).
- Packaged pair: YuNet `face_detection_yunet_2026may.onnx` and SFace
  `face_recognition_sface_2021dec_int8.onnx`. Validate on OpenCV 5 before adding model combinations
  or runtime path overrides.
- `ConfigureFaceModelEnvironment()` installs `InstallOpenCvLogFilter()`, matching exact level,
  source file, function, and message on OpenCV 5.0.0–5.1.0 only (history and 5.0/5.1 SFace cosine
  delta are commented in `include/vision/face_model/internal.hpp`). Retain for 5.0.0–5.1.0 even
  after upstream fixes; extend the range only after confirming a newer release emits the warning,
  and keep the filter narrow.

## Invariants

- Validate `cv::Mat` frames (`frame_validation.hpp`) before transformation or inference.
- Keep raw YuNet output rows inside face-model/detection internals; distinguish zero detections from
  inference or invalid-output failures.
- Keep orchestration tests (`PreviewEngine`, etc.) ONNX-free via injected inference; reserve
  model-backed coverage for `FaceModel` tests.
