# PROJECT KNOWLEDGE BASE

**Generated:** 2026-05-08
**Commit:** bc03695
**Branch:** refactor

## OVERVIEW

Linux facial recognition auth (PAM + native C++ CLI/runtime). Beta version. OpenCV DNN YuNet detector + SFace encoder, CLI management, recorder driver (OpenCV/V4L2).

## STRUCTURE

```text
howdy-next/
├── howdy/src/           # Product-facing sources/resources
│   ├── native/          # Native C++ sources (CLI, core, recorder, storage)
│   └── autocomplete/    # Shell completion template
├── include/howdy/       # Public/native headers (.hpp)
├── pam/                 # PAM module (separate build unit)
├── pam-config/          # PAM config template (separate from module)
├── config/              # Config template files
└── .forgejo/workflows/  # Forgejo CI (migrated from GitHub Actions)
```

## WHERE TO LOOK

| Task            | Location                        | Notes                              |
| --------------- | ------------------------------- | ---------------------------------- |
| CLI commands    | `howdy/src/native/cli/`        | Native subcommands                 |
| Face comparison | `howdy/src/native/compare.cpp` | Native recognition engine          |
| Camera drivers  | `howdy/src/native/recorders/`   | Native OpenCV capture             |
| PAM module      | `pam/`                          | C++ auth, main.cc                  |
| Auth config     | `config/config.ini`             | device_path, thresholds, timeout   |

## CODE MAP (Key Symbols)

| Symbol              | Type   | Location                              | Role                               |
| ------------------- | ------ | ------------------------------------- | ---------------------------------- |
| VideoCapture        | class  | native/recorders/video_capture.hpp   | OpenCV camera wrapper              |
| FaceModel           | class  | native/core/face_model.hpp          | YuNet detector + SFace encoder     |
| UserModels          | module | native/storage/user_models.hpp      | Native model file loading/writing   |
| howdy               | binary | native/howdy.cpp                    | CLI dispatcher                     |
| howdy-compare       | binary | native/compare.cpp                  | PAM compare executable             |

## CONVENTIONS (Deviations from Standard)

- **Build**: Meson (not CMake), subproject pattern (`if meson.is_subproject()`)
- **C++**: C++17/20-native migration with Meson/Ninja build
- **Testing**: Meson `test()` targets for native binaries
- **Config**: INI without comment header, comments above each key (config.ini)
- **Model validation**: native downloader checks for LFS pointers/HTML errors
- **Models**: Use the native downloader and packaged ONNX paths

## ANTI-PATTERNS (THIS PROJECT)

- **MUST NOT** modify PAM C++ auth flow without understanding auth behavior
- **MUST NOT** change config.ini format (CLI parses with configparser)
- **MUST NOT** use non-atomic config rewrites

## UNIQUE STYLES

- Recorder selection: VideoCapture factory instantiates based on device
- Config: INI format with native C++ read/write helpers
- PAM exit: Wait for user input (enter), do NOT auto-terminate
- OpenCV/V4L2: no FFmpeg backend in native runtime

## COMMANDS

```bash
# Build (Meson/Ninja)
meson setup build && ninja -C build

# Run native tests
meson test -C build native-capture-smoke-help native-compare-help --print-errorlogs

# CLI
howdy add <user>     # Add face
howdy test           # Test camera
howdy list           # List users
howdy disable        # Disable auth
howdy download-models # Download ONNX models from HuggingFace
```

## NOTES

- V4L2 device path: `/dev/video0`
- No pyproject.toml (meson-only, no pip packaging)
- Arch Linux: ONNX models excluded from PKGBUILD (downloaded at runtime)
- `backend = opencv_dnn_sface` config key removed (only one backend now)
