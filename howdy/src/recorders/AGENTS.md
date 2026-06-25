# Recorder Knowledge Base

**Updated:** 2026-06-25

## OVERVIEW

Native OpenCV camera capture layer.

## Where to Look

| File                                       | Role                                                                                                |
| ------------------------------------------ | --------------------------------------------------------------------------------------------------- |
| `howdy/src/recorders/video_capture.cpp`    | OpenCV camera wrapper used by CLI and compare flows                                                 |
| `howdy/include/cli/enrollment_capture.hpp` | Enrollment `capture_enrollment_sample()` template with brightness classification and face detection |

## Conventions

- `set()` and `get()` mirror OpenCV `CAP_PROP_FRAME_WIDTH` and `CAP_PROP_FRAME_HEIGHT`.
- `grab()` and `read()` match OpenCV `VideoCapture` API.
- Callers must handle open/read failures explicitly.
- Capture settings should be derived from typed `VideoConfig`, not raw `ConfigReader` reads.
- Keep recorder code aligned with repo-wide C++23 and tabs style.
- Enrollment capture flow uses reusable `capture_enrollment_sample()`
  template; classifiers for black/dark/empty frames in
  `enrollment_capture.hpp`.
