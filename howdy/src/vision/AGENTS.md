# Vision Guidelines

Read [`../../AGENTS.md`](../../AGENTS.md) first.

## Model Baseline

Current production inference uses the OpenCV 5 New DNN graph engine, pinned through
`OPENCV_FORCE_DNN_ENGINE` (`ForcedDnnEngine()`: `ENGINE_NEW` on OpenCV 5.0, `ENGINE_OPENCV` on 5.1+
after the engine enum was renumbered and the Classic engine removed). The packaged pair is YuNet
`face_detection_yunet_2026may.onnx` and SFace `face_recognition_sface_2021dec_int8.onnx`.

Do not add model combinations or runtime model-path overrides without fresh OpenCV 5 validation.

OpenCV 5.0.0 and 5.1.0 print an upstream `setPreferableTarget` warning ("Targets are not supported
by the new graph engine for now") when YuNet and SFace load. The warning is cosmetic: inference
still runs on the New graph engine. Upstream opencv/opencv#29457 silenced it for `DNN_TARGET_CPU`,
but opencv/opencv#29286 reintroduced it for builds without CUDA before 5.1.0 was tagged.

`ConfigureFaceModelEnvironment()` installs `InstallOpenCvLogFilter()`, which drops only that record
(level, source file, function, and message must all match) on OpenCV 5.0.0 through 5.1.0
(`kFiltersOpenCvTargetWarning`) and forwards every other OpenCV log record with OpenCV's own
formatting. Extend the version range only after confirming a newer release still prints the
warning; remove the filter once supported releases are fixed upstream. Do not broaden it into log
level changes or other message suppression.

OpenCV 5.1 SFace embeddings differ slightly from 5.0 (cosine about 0.99 for the same input), while
YuNet detections and the preprocessing pipeline are bit-identical. Enrollments made on 5.0 remain
usable on 5.1; scores sitting right at `sface_threshold` may move by a few hundredths.

## Ownership

- `frame_validation.hpp`: generic `cv::Mat` shape/type/channel validation
- `frame_processing.*`: CLAHE, rotation, brightness analysis, and resize scaling
- `face_detection.*`: detector-output parsing into semantic results
- `FaceModel`: detector/recognizer initialization and model-backed inference
- `VideoCapture`: allowed device paths, open/read/configuration, warm-up, error mapping
- `CompareEngine` / `PreviewEngine`: orchestration on semantic inference adapters

## Rules

Shared face metrics and capture-device policy belong under `include/support/`, not vision. Vision may
consume them but must not own contracts needed independently by config, storage, compare, or CLI.

Validate frames before transformation or inference. Do not expose raw YuNet rows outside
face-model/detection internals. Distinguish zero detections from inference/invalid-output failures.

Use typed `VideoConfig`/`FaceConfig`. Keep OpenCV includes as system includes. Tests that exercise
orchestration should inject inference and remain ONNX-free; use FaceModel tests for model-backed
behavior.
