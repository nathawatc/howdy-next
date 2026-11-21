# Vision Guidelines

Read [`../../AGENTS.md`](../../AGENTS.md) first.

## Model Baseline

Current production inference uses the OpenCV 5 New DNN graph engine, pinned through
`OPENCV_FORCE_DNN_ENGINE` (`ForcedDnnEngine()` in `include/vision/face_model/internal.hpp`). The
packaged pair is YuNet `face_detection_yunet_2026may.onnx` and SFace
`face_recognition_sface_2021dec_int8.onnx`.

Do not add model combinations or runtime model-path overrides without fresh OpenCV 5 validation.

`ConfigureFaceModelEnvironment()` installs `InstallOpenCvLogFilter()`, which drops only the OpenCV
target warning (level, source file, function, and message must all match) on OpenCV 5.0.0 through
5.1.0 and forwards every other record. Keep that filter for those releases even after upstream
ships a fix, and extend the range only after confirming a newer release still prints the warning.
Do not broaden it into log-level changes or other message suppression. The warning's history, the
upstream status, and the SFace embedding difference between 5.0 and 5.1 are commented in
`include/vision/face_model/internal.hpp`.

## Ownership

- `frame_validation.hpp`: generic `cv::Mat` shape/type/channel validation
- `frame_processing.*`: CLAHE, rotation, brightness analysis, and resize scaling
- `face_detection.*`: detector-output parsing into semantic results
- `FaceModel`: detector/recognizer initialization and model-backed inference
- `VideoCapture`: allowed device paths, open/read/configuration, warm-up, error mapping
- `PreviewEngine`: orchestration on semantic inference adapters (`CompareEngine` lives in
  `src/compare/engine.*`)

## Rules

Shared face metrics and capture-device policy belong under `include/support/`, not vision. Vision may
consume them but must not own contracts needed independently by config, storage, compare, or CLI.

Validate frames before transformation or inference. Do not expose raw YuNet rows outside
face-model/detection internals. Distinguish zero detections from inference/invalid-output failures.

Use typed `VideoConfig`/`FaceConfig`. Keep OpenCV includes as system includes. Tests that exercise
orchestration should inject inference and remain ONNX-free; use FaceModel tests for model-backed
behavior.
