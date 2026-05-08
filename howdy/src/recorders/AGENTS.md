# AGENTS.md - Howdy/src/recorders/

## OVERVIEW

Camera capture abstraction with factory pattern. VideoCapture factory selects ffmpeg_reader or
pyv4l2_reader based on config.

## WHERE TO LOOK

| File               | Role                                                         |
| ------------------ | ------------------------------------------------------------ |
| `video_capture.py` | Factory class; instantiates appropriate recorder from config |
| `ffmpeg_reader.py` | FFmpeg subprocess + ffmpeg-python probe; fallback path       |
| `pyv4l2_reader.py` | Direct ioctl V4L2 capture; falls back to ffmpeg.probe()      |
| `v4l2.py`          | DEPRECATED constant mappings for V4L2 ioctl; do not modify   |

## CONVENTIONS

- Both readers expose `probe()` method for camera detection (resolution discovery)
- `set()`/`get()` mirror OpenCV CAP_PROP_FRAME_WIDTH/HEIGHT constants
- `grab()` / `read()` / `record()` match OpenCV VideoCapture API
- Factory selects reader: `ffmpeg_reader` if ffmpeg available, else `pyv4l2_reader`
- FFmpeg probe returns `int` for height/width (cast with `int()`)
- Config path can be string or pre-parsed configparser object

## ANTI-PATTERNS

- **MUST NOT** modify v4l2.py constants (deprecated, sourced from Linux kernel headers)
- **MUST NOT** assume probe() always succeeds; both readers have fallback logic
