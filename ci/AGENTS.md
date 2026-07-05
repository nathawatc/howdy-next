# CI Container Rules

**Updated:** 2026-07-05

## Image Identity

- Registry image: `codeberg.org/nathawat/howdy-next/ci-1:latest`
- CI config must reference `codeberg.org/nathawat/howdy-next/ci-1:latest`.
- Use `latest` tag for CI image. Re-push replaces previous image for that tag.

## Containerfile

- Use fully qualified base image: `FROM docker.io/library/debian:sid-slim`
- Keep OCI metadata only in `Containerfile`. Do not repeat identical
  `org.opencontainers.image.*` labels in `podman build` commands.
- Prefer Debian sid packages. Do not add third-party repos or source builds
  unless required dependency or API is missing in sid.
- Keep package install minimal and remove APT lists in same layer:
  `rm -rf /var/lib/apt/lists/*`.
- Keep container non-interactive with `DEBIAN_FRONTEND=noninteractive`.

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
