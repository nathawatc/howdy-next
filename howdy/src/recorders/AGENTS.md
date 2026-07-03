# Recorder Knowledge Base

**Updated:** 2026-07-03

## Overview

Native OpenCV camera capture layer.

## Where to Look

- `howdy/src/recorders/video_capture.cpp`: OpenCV camera wrapper used by CLI
  and compare flows.
- `howdy/include/cli/enrollment_capture.hpp`: enrollment
  `capture_enrollment_sample()` template with brightness classification and
  face detection.

## Conventions

- `set()` and `get()` mirror OpenCV `CAP_PROP_FRAME_WIDTH` and
  `CAP_PROP_FRAME_HEIGHT`.
- `grab()` and `read()` match OpenCV `VideoCapture` API.
- Callers must handle open/read failures explicitly.
- Validate captured `cv::Mat` through `common/frame_validation.hpp` before
  color conversion or caller-visible processing.
- Capture settings should come from typed `VideoConfig`, not raw
  `ConfigReader` reads.
- Keep recorder code aligned with repo-wide C++23 and tabs style.
- Enrollment capture flow uses reusable `capture_enrollment_sample()` template;
  classifiers for black/dark/empty frames live in `enrollment_capture.hpp`.
