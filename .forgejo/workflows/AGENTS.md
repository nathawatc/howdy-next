# CI Container Rules

**Updated**: 2026-07-01

## Image Identity

- Registry image: `codeberg.org/nathawat/howdy-next/ci-1`
- CI configuration must reference: `codeberg.org/nathawat/howdy-next/ci-1:latest`.
- Use `latest` as the CI image tag. Re-pushing it replaces the previous
  image for that tag.

## Containerfile

- Use a fully qualified base-image reference. Keep:
  `FROM docker.io/library/debian:sid-slim`
- Keep OCI metadata only in `Containerfile`. Do not repeat identical
  `org.opencontainers.image.*` labels in `podman build` commands.
- Prefer Debian sid packages. Do not add third-party repositories or source
  builds unless a required dependency/API is unavailable in sid.
- Keep package installation minimal and remove APT lists in the same layer:
  `rm -rf /var/lib/apt/lists/*`.
- Keep the container non-interactive with `DEBIAN_FRONTEND=noninteractive`.

## Validation

When changing packages, the base image, or build tools:

1. Build from `ci/`.
2. Verify Meson can configure the project in the image.
3. Run the affected native test targets.
4. Check that the resolved yyjson package version satisfies Meson.

## Publishing

- Authenticate with `podman login codeberg.org`.
- Use `--pull=always` for CI-image rebuilds.
- Use `--no-cache` only when a clean rebuild is intended.
- Push `codeberg.org/nathawat/howdy-next/ci-1:latest`.
- Re-pushing `:latest` replaces the previous image for that tag.
