# CI Container Rules

**Updated:** 2026-08-15

## Workflow and Image

- `.forgejo/workflows/ci.yml` runs one release configure/build/test job in
  `codeberg.org/nathawat/howdy-next/ci-1:latest`.
- Keep that image identity and mutable `latest` tag aligned between the
  workflow and image publishing.
- Build the image from `ci/Containerfile` with fully qualified base image
  `docker.io/library/archlinux:latest`.

## Containerfile Policy

- Install dependencies through pacman; do not build OpenCV from source.
- Keep compiler/build tooling at `gcc`, `make`, and `cmake>=3.31`.
- Keep `glibc>=2.34` for
  `posix_spawn_file_actions_addclosefrom_np()`.
- Keep `opencv>=5.0.0`, `yyjson>=0.12.0`, and `libinih>=59` aligned with the
  root CMake requirements.
- Keep `qt6-base`; OpenCV HighGUI links against Qt 6 in this image.
- Keep package installation minimal and clear pacman caches in the same layer.
- Keep OCI metadata in `Containerfile`; do not duplicate its labels in build
  commands.

## Validation

When changing the container, workflow, or build dependencies:

1. From `ci/`, build with `podman build --pull=always -t
codeberg.org/nathawat/howdy-next/ci-1:latest -f Containerfile .`.
2. Configure and build the repository with the appropriate CMake preset, using
   `--parallel "$(nproc)"` and `HOWDY_WARNINGS_AS_ERRORS=ON` for CI parity.
3. Run affected CTest tests; do not add an unnecessary full matrix.
4. Check glibc, OpenCV, libinih, libcurl, and yyjson versions satisfy the root
   CMake requirements.
