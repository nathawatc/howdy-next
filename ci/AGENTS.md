# CI Container Rules

**Updated:** 2026-07-06

## Image Identity

- Registry image: `codeberg.org/nathawat/howdy-next/ci-1:latest`
- CI config must reference `codeberg.org/nathawat/howdy-next/ci-1:latest`.
- Use `latest` tag for CI image. Re-push replaces previous image for that tag.

## Containerfile

- Use fully qualified base image: `docker.io/library/archlinux:latest`.
- Install dependencies through pacman. Do not build OpenCV from source.
- Install `gcc`; Arch `base` image does not include a C++ compiler.
- Require `opencv >= 5.0.0` from Arch stable repositories.
- Install `qt6-base`; Arch OpenCV HighGUI links against Qt 6.
- Keep package install minimal. Clear pacman package and sync caches in same
  layer with `pacman -Scc --noconfirm`.
- Keep OCI metadata only in `Containerfile`. Do not repeat identical
  `org.opencontainers.image.*` labels in `podman build` commands.

## Validation

When changing packages, base image, or build tools:

1. Build from `ci/`.
2. Verify Meson can configure project in image.
3. Run affected native test targets.
4. Check resolved yyjson package version satisfies Meson.

## Publishing

- Authenticate with `podman login codeberg.org`.
- Use `--pull=always` for CI-image rebuilds.
- Use `--no-cache` only when a clean rebuild is intended.
- Push `codeberg.org/nathawat/howdy-next/ci-1:latest`.
- Re-pushing `:latest` replaces previous image for that tag.
