# Vision Guidelines

**Updated:** 2026-09-13

Read [`../../AGENTS.md`](../../AGENTS.md) first.

## Model Baseline

Current production inference uses OpenCV 5 DNN with `OPENCV_FORCE_DNN_ENGINE=2`. The packaged pair
is YuNet `face_detection_yunet_2026may.onnx` and SFace `face_recognition_sface_2021dec_int8.onnx`.

Do not add model combinations or runtime model-path overrides without fresh OpenCV 5 validation.
OpenCV versions before 5.1 may print an upstream `setPreferableTarget` warning with the new graph
engine; do not add a local workaround for that warning.

## Ownership

- `frame_validation.hpp`: generic `cv::Mat` shape/type/channel validation
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
