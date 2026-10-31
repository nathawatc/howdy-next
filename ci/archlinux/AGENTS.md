# Arch Linux CI Guidelines

Read the [repository guidelines](../../AGENTS.md) first.

## Scope

Covers `ci/archlinux/` and `.github/workflows/archlinux.yml`.

## Rules

- Keep `.github/workflows/archlinux.yml` aligned with `ghcr.io/nathawatc/ci-archlinux:latest`.
- Build from `docker.io/library/archlinux:latest`.
- Install dependencies with pacman; do not build OpenCV from source in CI.
- Keep dependency floors aligned with root CMake requirements, including CMake, glibc, OpenCV,
  yyjson, libinih, and Qt 6 requirements.
- Keep the image minimal and clear pacman caches in the same layer.
- Keep OCI labels in `Containerfile`, not duplicated in wrapper commands.
- E2E tests are excluded. GitHub Actions runners do not expose the required container privileges.

## Local CI

Run from the repository root:

```sh
podman run --rm \
  -v "$PWD:/workspace:ro" \
  --tmpfs /workspace/build:rw,exec,notmpcopyup \
  -w /workspace \
  ghcr.io/nathawatc/ci-archlinux:latest bash -c '
    set -e
    cmake --preset release -DHOWDY_WARNINGS_AS_ERRORS=ON
    cmake --build --preset release --parallel "$(nproc)"
    ctest --preset release
  '
```

Rebuild the CI image when dependencies or the image recipe change. Image build and publishing
commands are maintainer-only and live in [`../README.md`](../README.md).
