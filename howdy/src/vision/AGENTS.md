# Core Face Pipeline Guidelines

**Updated:** 2026-08-15

## Scope

Face-model, detection, embedding, frame-processing, and camera-capture code
under `howdy/src/vision/` and its public headers under `howdy/include/vision/`.

## OpenCV 5 DNN Compatibility

`FaceModel` uses OpenCV 5 New graph engine (`OPENCV_FORCE_DNN_ENGINE=2`) for
bundled face models. Legacy `*_int8bq` models are not supported on this path.
The repository's canonical packaged pair is
`face_detection_yunet_2026may.onnx` and
`face_recognition_sface_2021dec_int8.onnx`.

Validated compatibility:

| Engine         | YuNet            | SFace            | Result                                 |
| -------------- | ---------------- | ---------------- | -------------------------------------- |
| New (`=2`)     | `2023mar_int8bq` | `2021dec_int8bq` | Fails in `FaceRecognizerSF::feature()` |
| New (`=2`)     | `2026may`        | `2021dec_int8bq` | Fails in `FaceRecognizerSF::feature()` |
| New (`=2`)     | `2026may`        | `2021dec` FP32   | Passes; embedding `1x128 CV_32F`       |
| New (`=2`)     | `2026may`        | `2021dec_int8`   | Passes; embedding `1x128 CV_32F`       |
| Classic (`=1`) | `2023mar_int8bq` | `2021dec_int8bq` | Fails during detector initialization   |
| Classic (`=1`) | `2026may`        | `2021dec_int8bq` | Passes; embedding `1x128 CV_32F`       |

OpenCV releases before 5.1 may print upstream `setPreferableTarget` warnings
with the New graph engine. They are not Howdy failures. The upstream patch is
merged and will fix this warning in OpenCV 5.1; do not add a local workaround.
Do not add model combinations or runtime model-path overrides without fresh
OpenCV 5 validation and corresponding repository evidence.

## Ownership Boundaries

- `vision/frame_validation.hpp` owns generic `cv::Mat` shape, dimension,
  channel, and element-type validation. Validate before color conversion,
  transformation, or inference.
- `vision/face_detection.hpp` owns the detector-output parsing boundary.
  `parse_yunet_detections()` returns semantic `FaceDetectionResult`; callers
  distinguish valid zero detections from inference and invalid-output failures.
- `FaceModel` owns actual OpenCV detector/recognizer initialization and
  inference. It exposes semantic detections, encodings, and matches rather than
  raw YuNet rows.
- `CompareEngine` and `PreviewEngine` consume semantic inference adapters;
  orchestration should not depend directly on OpenCV detector output layout.
- `VideoCapture` owns camera-wrapper behavior: allowed device paths, OpenCV
  open/read/configuration, warm-up, error mapping, and frame validation.

## Conventions and Tests

- Use typed `VideoConfig`/`FaceConfig` values, not raw `ConfigReader` reads.
- Keep `howdy_opencv` include directories marked as system includes.
- Tests that inject inference should remain ONNX-free when they exercise
  orchestration, frame handling, or semantic result mapping rather than OpenCV
  model execution. Use `FaceModel` tests for actual model-backed behavior.
- Callers handle camera open/read failures explicitly and do not treat a
  captured frame as valid until `frame_validation.hpp` accepts it.
