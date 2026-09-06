# CI Guidelines

**Updated:** 2026-09-06

Read the [repository guidelines](../AGENTS.md) first.

## Scope

Covers `ci/Containerfile` and CI workflow compatibility.

## Rules

- Keep `.forgejo/workflows/ci.yml` aligned with `codeberg.org/nathawat/howdy-next/ci-1:latest`.
- Build from `docker.io/library/archlinux:latest`.
- Install dependencies with pacman; do not build OpenCV from source in CI.
- Keep dependency floors aligned with root CMake requirements, including CMake, glibc, OpenCV,
  yyjson, libinih, and Qt 6 requirements.
- Keep the image minimal and clear pacman caches in the same layer.
- Keep OCI labels in `Containerfile`, not duplicated in wrapper commands.

## Validation

For CI/container changes:

```sh
cd ci
podman build --pull=always -t codeberg.org/nathawat/howdy-next/ci-1:latest -f Containerfile .
```

Then configure/build with warnings-as-errors parity and run affected CTest suites. Do not expand the
CI matrix without a concrete compatibility need.
