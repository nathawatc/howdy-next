# AGENTS.md - Howdy/src/lib/recorders/

## OVERVIEW

Native OpenCV camera capture layer.

## WHERE TO LOOK

| File                                    | Role                                   |
| --------------------------------------- | -------------------------------------- |
| `howdy/src/lib/cpp/recorders/video_capture.cpp` | OpenCV camera wrapper used by native CLI and compare flow |

## CONVENTIONS

- `set()` / `get()` mirror OpenCV `CAP_PROP_FRAME_WIDTH` / `CAP_PROP_FRAME_HEIGHT`.
- `grab()` / `read()` match the OpenCV `VideoCapture` API.
- Callers must handle open/read failures explicitly.
