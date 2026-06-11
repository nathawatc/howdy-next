# Recorder Knowledge Base

**Updated:** 2026-06-11

## OVERVIEW

Native OpenCV camera capture layer.

## Where To Look

| File                                    | Role                                                |
| --------------------------------------- | --------------------------------------------------- |
| `howdy/src/recorders/video_capture.cpp` | OpenCV camera wrapper used by CLI and compare flows |

## Conventions

- `set()` and `get()` mirror OpenCV `CAP_PROP_FRAME_WIDTH` and `CAP_PROP_FRAME_HEIGHT`.
- `grab()` and `read()` match OpenCV `VideoCapture` API.
- Callers must handle open/read failures explicitly.
- Capture settings should be derived from typed `VideoConfig`, not raw `ConfigReader` reads.
- Keep recorder code aligned with repo-wide C++23 and tabs style.
