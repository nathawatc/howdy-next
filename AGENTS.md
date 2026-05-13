# PROJECT KNOWLEDGE BASE

**Generated:** 2026-05-08
**Commit:** bc03695
**Branch:** refactor

## OVERVIEW

Linux facial recognition auth (PAM + Python). Beta version. OpenCV DNN YuNet detector + SFace encoder, CLI management, recorder drivers (V4L2/FFmpeg).

## STRUCTURE

```text
howdy-next/
├── howdy/src/           # Project sources/resources
│   ├── lib/             # Python source (CLI, recorders, core)
│   │   ├── cli/         # CLI subcommands (add, test, set, disable...)
│   │   ├── core/        # Face detection/encoding
│   │   └── recorders/   # Camera readers (ffmpeg, video_capture)
│   ├── config/          # Config template
│   └── pam/             # C++ PAM authentication module
├── tests/               # pytest test infrastructure
└── .forgejo/workflows/  # Forgejo CI (migrated from GitHub Actions)
```

## WHERE TO LOOK

| Task            | Location                   | Notes                                  |
| --------------- | -------------------------- | -------------------------------------- |
| CLI commands    | `howdy/src/lib/cli/`       | add.py, test.py, set.py, disable.py... |
| Face comparison | `howdy/src/lib/compare.py` | Core recognition engine, 444 lines     |
| Camera drivers  | `howdy/src/lib/recorders/` | ffmpeg_reader.py                       |
| PAM module      | `howdy/src/pam/`           | C++ auth, main.cc                      |
| Auth config     | `howdy/src/config/config.ini` | device_path, certainty, timeout    |

## CODE MAP (Key Symbols)

| Symbol              | Type   | Location                        | Role                                  |
| ------------------- | ------ | ------------------------------- | ------------------------------------- |
| VideoCapture        | class  | lib/recorders/video_capture.py | Factory for recorder selection        |
| ffmpeg_reader       | class  | lib/recorders/ffmpeg_reader.py | FFmpeg-based camera capture           |
| compare             | module | lib/compare.py                 | Face comparison engine                |
| main.py             | entry  | main.py                        | Main CLI entry point                  |
| bad_model_download  | func   | lib/core/detector.py           | Validates ONNX model (LFS/HTML check) |
| FaceModel           | class  | lib/core/detector.py           | YuNet detector + SFace encoder        |
| YUNET_URL/SFACE_URL | const  | lib/core/detector.py           | HuggingFace model download URLs       |

## CONVENTIONS (Deviations from Standard)

- **Build**: Meson (not CMake), subproject pattern (`if meson.is_subproject()`)
- **Python**: 3.14 target, `from __future__ import annotations` for all typed files
- **Type hints**: Incremental approach, NOT strict mypy mode
- **Testing**: pytest with `conftest.py` fixture pattern, mock_config fixture
- **i18n**: `i18n.py` expects `locales/` dir but project has `po/` files instead
- **Config**: INI without comment header, comments above each key (config.ini)
- **Model validation**: `bad_model_download()` checks for LFS pointers/HTML errors
- **Model URLs**: Use `YUNET_URL`/`SFACE_URL` constants from `core.detector`, not hardcoded

## ANTI-PATTERNS (THIS PROJECT)

- **MUST NOT** modify PAM C code without understanding auth flow
- **MUST NOT** change config.ini format (CLI parses with configparser)
- **MUST NOT** use `fileinput.input()` for config editing (race condition - use atomic tempfile pattern)
- **MUST NOT** use `_thread` module (deprecated) - use `threading` instead

## UNIQUE STYLES

- Recorder selection: VideoCapture factory instantiates based on device
- Config: INI format, CLI modules import `i18n` for translations
- PAM exit: Wait for user input (enter), do NOT auto-terminate
- v4l2: Video format option (used by ffmpeg), NOT a recorder backend

## COMMANDS

```bash
# Build (Meson/Ninja)
meson setup build && ninja -C build

# Run tests (pytest)
python3 -m pytest tests/ -v

# CLI
howdy add <user>     # Add face
howdy test           # Test camera
howdy list           # List users
howdy disable        # Disable auth
howdy download-models # Download ONNX models from HuggingFace
```

## NOTES

- V4L2 device path: `/dev/video0`
- FFmpeg probe returns `int` for height/width (NOT string) - cast with `int()`
- No pyproject.toml (meson-only, no pip packaging)
- Arch Linux: ONNX models excluded from PKGBUILD (downloaded at runtime)
- `backend = opencv_dnn_sface` config key removed (only one backend now)
