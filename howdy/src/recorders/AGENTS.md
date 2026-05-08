# AGENTS.md - Howdy/src/recorders/

## OVERVIEW

Camera capture abstraction with factory pattern. VideoCapture factory selects ffmpeg_reader or
opencv based on config.

## WHERE TO LOOK

| File               | Role                                                         |
| ------------------ | ------------------------------------------------------------ |
| `ffmpeg_reader.py` | FFmpeg-based camera capture; fallback path                   |
| `video_capture.py` | Factory class; instantiates appropriate recorder from config |

## CONVENTIONS

- Both readers expose `probe()` method for camera detection (resolution discovery)
- `set()`/`get()` mirror OpenCV CAP_PROP_FRAME_WIDTH/HEIGHT constants
- `grab()` / `read()` / `record()` match OpenCV VideoCapture API
- Factory selects reader: `ffmpeg_reader` if ffmpeg available, else `opencv`
- FFmpeg probe returns `int` for height/width (cast with `int()`)
- Config path can be string or pre-parsed configparser object

## ANTI-PATTERNS

- **MUST NOT** assume probe() always succeeds; both readers have fallback logic
