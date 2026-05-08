# PROJECT KNOWLEDGE BASE

**Generated:** 2026-05-08
**Commit:** bc03695
**Branch:** refactor

## OVERVIEW

Linux facial recognition auth (PAM + Python). Beta version. Face comparison engine, CLI management,
recorder drivers (V4L2/FFmpeg).

## STRUCTURE

```text
howdy-next/
├── howdy/src/           # Python source (CLI, recorders, PAM auth)
│   ├── cli/             # CLI subcommands (add, test, set, disable...)
│   ├── recorders/       # Camera readers (ffmpeg, video_capture)
│   └── pam/             # C++ PAM authentication module
├── tests/               # pytest test infrastructure
└── .forgejo/workflows/  # Forgejo CI (migrated from GitHub Actions)
```

## WHERE TO LOOK

| Task            | Location               | Notes                                  |
| --------------- | ---------------------- | -------------------------------------- |
| CLI commands    | `howdy/src/cli/`       | add.py, test.py, set.py, disable.py... |
| Face comparison | `howdy/src/compare.py` | Core recognition engine, 444 lines     |
| Camera drivers  | `howdy/src/recorders/` | ffmpeg_reader.py              |
| PAM module      | `howdy/src/pam/`       | C++ auth, main.cc                      |
| Auth config     | `howdy/src/config.ini` | device_path, certainty, timeout        |

## CODE MAP (Key Symbols)

| Symbol        | Type   | Location                   | Role                           |
| ------------- | ------ | -------------------------- | ------------------------------ |
| VideoCapture  | class  | recorders/video_capture.py | Factory for recorder selection |
| ffmpeg_reader | class  | recorders/ffmpeg_reader.py | FFmpeg-based camera capture    |
| compare       | module | compare.py                 | Face comparison engine         |
| cli.py        | entry  | cli.py                     | Main CLI entry point           |

## CONVENTIONS (Deviations from Standard)

- **Build**: Meson (not CMake), subproject pattern (`if meson.is_subproject()`)
- **Python**: 3.14 target, `from __future__ import annotations` for all typed files
- **Type hints**: Incremental approach, NOT strict mypy mode
- **Testing**: pytest with `conftest.py` fixture pattern, mock_config fixture
- **i18n**: `i18n.py` expects `locales/` dir but project has `po/` files instead

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
```

## NOTES

- V4L2 device path: `/dev/video0`
- FFmpeg probe returns `int` for height/width (NOT string) - cast with `int()`
- rubberstamps/ uses dynamic plugin loading via `importlib.util`
- No pyproject.toml (meson-only, no pip packaging)
