# Arch Linux CI Guidelines

**Updated:** 2026-09-14

Read the [repository guidelines](../../AGENTS.md) first.

## Scope

Covers `ci/archlinux/` and `.forgejo/workflows/archlinux.yml`.

## Rules

- Keep `.forgejo/workflows/archlinux.yml` aligned with `codeberg.org/nathawat/howdy-next/ci-archlinux:latest`.
- Build from `docker.io/library/archlinux:latest`.
- Install dependencies with pacman; do not build OpenCV from source in CI.
- Keep dependency floors aligned with root CMake requirements, including CMake, glibc, OpenCV,
  yyjson, libinih, and Qt 6 requirements.
- Keep the image minimal and clear pacman caches in the same layer.
- Keep OCI labels in `Containerfile`, not duplicated in wrapper commands.

## Local CI

Run from the repository root:

```sh
podman run --rm \
  -v "$PWD:/workspace:ro" \
  --tmpfs /workspace/build:rw,exec \
  -w /workspace \
  codeberg.org/nathawat/howdy-next/ci-archlinux:latest bash -c '
    set -e
    cmake --preset release -DHOWDY_WARNINGS_AS_ERRORS=ON
    cmake --build --preset release --parallel "$(nproc)"
    ctest --preset release
  '
```

Image build and publishing commands are maintainer-only and live in [`../README.md`](../README.md).
