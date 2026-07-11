# Core Face Pipeline Guidelines

**Updated:** 2026-07-06

## Scope

Core face-model, detection, and embedding code under `howdy/src/core/`.

## OpenCV 5 DNN Engine Compatibility

`FaceModel` uses OpenCV 5 New graph engine (`OPENCV_FORCE_DNN_ENGINE=2`) for
bundled face models. Legacy `*_int8bq` models are not supported on this path.

Verified compatibility:

| Engine         | YuNet            | SFace            | Result                                 |
| -------------- | ---------------- | ---------------- | -------------------------------------- |
| New (`=2`)     | `2023mar_int8bq` | `2021dec_int8bq` | Fails in `FaceRecognizerSF::feature()` |
| New (`=2`)     | `2026may`        | `2021dec_int8bq` | Fails in `FaceRecognizerSF::feature()` |
| New (`=2`)     | `2026may`        | `2021dec` FP32   | Passes; embedding `1x128 CV_32F`       |
| New (`=2`)     | `2026may`        | `2021dec_int8`   | Passes; embedding `1x128 CV_32F`       |
| Classic (`=1`) | `2023mar_int8bq` | `2021dec_int8bq` | Fails during detector initialization   |
| Classic (`=1`) | `2026may`        | `2021dec_int8bq` | Passes; embedding `1x128 CV_32F`       |

OpenCV may still print upstream warnings while using New graph engine:

```text
[ WARN:0@0.006] global net_impl_backend.cpp:345 setPreferableTarget Targets are not supported by the new graph engine for now
[ WARN:0@0.027] global net_impl_backend.cpp:345 setPreferableTarget Targets are not supported by the new graph engine for now
```

These warnings are upstream-only. Not fixable in Howdy. OpenCV still runs with
New graph engine after warning.

## Conventions

- Canonical packaged OpenCV 5 model pair:
  `face_detection_yunet_2026may.onnx` and
  `face_recognition_sface_2021dec_int8.onnx`.
- Do not reintroduce runtime config model-path overrides or legacy
  `*_int8bq` combinations without fresh OpenCV 5 validation.
