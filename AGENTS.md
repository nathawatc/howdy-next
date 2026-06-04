# PROJECT KNOWLEDGE BASE

**Generated:** 2026-06-04
**Commit:** 051d84c
**Branch:** master

## OVERVIEW

Linux facial recognition auth for PAM + native C++ CLI/runtime. OpenCV DNN YuNet
detector with SFace encoder, Meson/Ninja build, Arch packaging support.

## STRUCTURE

```text
howdy-next/
├── howdy/src/           # Native runtime, CLI, config, core, storage, tests
├── howdy/include/       # Shared native headers
├── pam/                 # PAM module and tests
├── config/              # Packaged config template
└── archlinux/           # Arch PKGBUILD and packaging sources
```

## WHERE TO LOOK

| Task            | Location                        | Notes                                      |
| --------------- | ------------------------------- | ------------------------------------------ |
| CLI commands    | `howdy/src/cli/`                | Native subcommands and editor/config flows |
| Face comparison | `howdy/src/compare.cpp`         | Native recognition engine                  |
| Camera drivers  | `howdy/src/recorders/`          | OpenCV/V4L2 capture wrapper                |
| Model handling  | `howdy/src/core/face_model.cpp` | YuNet/SFace loading and validation         |
| Config runtime  | `howdy/src/config/`             | Config reader, rewrite, path resolution    |
| PAM module      | `pam/`                          | Auth flow and status mapping               |

## CODE MAP

| Symbol          | Type   | Location                                    | Role                             |
| --------------- | ------ | ------------------------------------------- | -------------------------------- |
| `VideoCapture`  | class  | `howdy/include/recorders/video_capture.hpp` | OpenCV camera wrapper            |
| `FaceModel`     | class  | `howdy/include/core/face_model.hpp`         | Detector + encoder pipeline      |
| `UserModels`    | module | `howdy/include/storage/user_models.hpp`     | User model file loading/writing  |
| `CompareExit`   | enum   | `howdy/include/common/compare_exit.hpp`     | Compare binary exit codes        |
| `compare_logic` | module | `howdy/include/common/compare_logic.hpp`    | Frame resize + exception helpers |
| `compare_args`  | module | `howdy/include/common/compare_args.hpp`     | Compare arg parsing              |
| `howdy`         | binary | `howdy/src/howdy.cpp`                       | CLI dispatcher                   |
| `howdy-compare` | binary | `howdy/src/compare.cpp`                     | Auth compare executable          |

## CONVENTIONS

- Build with Meson/Ninja, not CMake.
- Config reads + writes atomic.
- PAM + config paths: secure ownership + directory checks.
- `/etc/howdy` installed `root:root` `0750`; `config.ini` `0640`.
- Model files validated before load; custom model paths absolute.
- Prefer shared helpers in `howdy/include/common/` + `howdy/include/config/`.

## ANTI-PATTERNS

- Don't change `config.ini` format.
- Don't bypass PAM auth flow without understanding session behavior.
- Don't replace atomic config rewrite logic with in-place edits.

## UNIQUE STYLES

- Recorder selection uses `VideoCapture` factory based on device path.
- PAM exits wait for user input after success; not auto-terminate.
- OpenCV/V4L2 runtime lacks FFmpeg backend.

## COMMANDS

```bash
meson setup build && ninja -C build
meson test -C build --print-errorlogs
howdy add <user>
howdy test
howdy list
howdy disable
howdy download-models
```

## NOTES

- Default camera device path: `/dev/video0`.
- Arch packaging installs config under `/etc/howdy`.
- `backend = opencv_dnn_sface` removed; only one backend remains.
- #12: `compare_resize_scale` prevents auth-frame upscaling; compare and camera capture paths convert OpenCV/std exceptions to controlled failures.
