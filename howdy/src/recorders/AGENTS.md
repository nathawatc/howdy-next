# RECORDER KNOWLEDGE BASE

**Generated:** 2026-05-14

## OVERVIEW

Native OpenCV camera capture layer.

## WHERE TO LOOK

| File                                    | Role                                                |
| --------------------------------------- | --------------------------------------------------- |
| `howdy/src/recorders/video_capture.cpp` | OpenCV camera wrapper used by CLI and compare flows |

## CONVENTIONS

- `set()` and `get()` mirror OpenCV `CAP_PROP_FRAME_WIDTH` and `CAP_PROP_FRAME_HEIGHT`.
- `grab()` and `read()` match the OpenCV `VideoCapture` API.
- Callers must handle open/read failures explicitly.
