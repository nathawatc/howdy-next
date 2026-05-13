# AGENTS.md - Howdy/src/lib/recorders/

## OVERVIEW

Camera capture abstraction using OpenCV only.

## WHERE TO LOOK

| File               | Role                                                         |
| ------------------ | ------------------------------------------------------------ |
| `video_capture.py` | OpenCV camera wrapper used by CLI and compare flow           |

## CONVENTIONS

- `set()`/`get()` mirror OpenCV CAP_PROP_FRAME_WIDTH/HEIGHT constants
- `grab()` / `read()` match OpenCV VideoCapture API
- Config path can be string or pre-parsed configparser object

## ANTI-PATTERNS

- **MUST NOT** assume camera open/read always succeeds; caller must handle exit 14 paths
