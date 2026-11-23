# Arch Linux CI Guidelines

Parent: [`../../AGENTS.md`](../../AGENTS.md). Maintainer image build/push:
[`../README.md`](../README.md).

## Scope

Covers `ci/archlinux/`, `.github/workflows/archlinux.yml`, and `.github/workflows/clang_tidy.yml`.

## Invariants

- Base `Containerfile` on `docker.io/library/archlinux:latest` and keep
  `.github/workflows/archlinux.yml` aligned with `ghcr.io/nathawatc/howdy-next/ci-archlinux:latest`.
- Install dependencies with pacman (never build OpenCV from source in CI), clear pacman caches in
  the same layer, and keep OCI labels in `Containerfile` rather than wrapper commands.
- Keep pacman dependency floors aligned with root `CMakeLists.txt` (CMake, glibc, OpenCV, yyjson,
  libinih, libcurl) and keep the `clang` floor aligned with `.clang-tidy`.
- Keep `qt6-base` (Arch's OpenCV HighGUI Qt 6 backend used by `howdy test`; no CMake target names
  Qt, so it looks unused) and keep the `pacman.conf` `usr/share/i18n/*` override plus `th_TH.UTF-8`
  locale generation (required by PAM translation tests).
- Keep `CCACHE_DIR` outside the workspace; only `main` saves the cache, while pull requests restore
  only.
- `clang_tidy.yml` reuses this image with `fetch-depth: 2` and `BASE=HEAD^1`, configures `debug`,
  and builds only `howdy_version_metadata`.
- Keep privileged E2E tests excluded from GitHub Actions runners (runners lack the required
  container privileges).
