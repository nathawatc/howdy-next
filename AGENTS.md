# PROJECT KNOWLEDGE BASE

**Generated:** 2026-05-14
**Commit:** bc03695
**Branch:** refactor

## OVERVIEW

Linux facial recognition auth for PAM plus native C++ CLI/runtime. OpenCV DNN YuNet
detector with SFace encoder, Meson/Ninja build, and Arch packaging support.

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

| Symbol          | Type   | Location                                    | Role                            |
| --------------- | ------ | ------------------------------------------- | ------------------------------- |
| `VideoCapture`  | class  | `howdy/include/recorders/video_capture.hpp` | OpenCV camera wrapper           |
| `FaceModel`     | class  | `howdy/include/core/face_model.hpp`         | Detector + encoder pipeline     |
| `UserModels`    | module | `howdy/include/storage/user_models.hpp`     | User model file loading/writing |
| `howdy`         | binary | `howdy/src/howdy.cpp`                       | CLI dispatcher                  |
| `howdy-compare` | binary | `howdy/src/compare.cpp`                     | Auth compare executable         |

## CONVENTIONS

- Build with Meson/Ninja, not CMake.
- Config reads and writes must stay atomic.
- PAM and config paths use secure ownership and directory checks.
- `/etc/howdy` is installed `root:root` with `0750`; `config.ini` is `0640`.
- Model files are validated before load; custom model paths must be absolute.
- Prefer shared helpers in `howdy/include/common/` and `howdy/include/config/`.

## ANTI-PATTERNS

- Do not change `config.ini` format.
- Do not bypass PAM auth flow without understanding session behavior.
- Do not replace atomic config rewrite logic with in-place edits.

## UNIQUE STYLES

- Recorder selection uses a `VideoCapture` factory based on device path.
- PAM exits wait for user input after success; they do not auto-terminate.
- OpenCV/V4L2 runtime has no FFmpeg backend.

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

- Default camera device path is `/dev/video0`.
- Arch packaging installs config under `/etc/howdy`.
- `backend = opencv_dnn_sface` was removed; only one backend remains.
