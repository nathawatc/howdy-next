# Arch Linux CI Guidelines

Read the [repository guidelines](../../AGENTS.md) first.

## Scope

Covers `ci/archlinux/`, `.github/workflows/archlinux.yml`, and `.github/workflows/clang_tidy.yml`.

## Rules

- Keep `.github/workflows/archlinux.yml` aligned with `ghcr.io/nathawatc/howdy-next/ci-archlinux:latest`.
- Build from `docker.io/library/archlinux:latest`.
- Install dependencies with pacman; do not build OpenCV from source in CI.
- Keep dependency floors aligned with root CMake requirements, including CMake, glibc, OpenCV,
  yyjson, libinih, and Qt 6 requirements, and keep the `rust` floor at the workspace
  `rust-version`.
- Keep the image minimal and clear pacman caches in the same layer.
- Keep OCI labels in `Containerfile`, not duplicated in wrapper commands.
- Keep the `clang` floor aligned with the LLVM version `.clang-tidy` is written for.
- Keep ccache storage on `CCACHE_DIR` outside the workspace. Only runs on `main` save the cache;
  pull requests restore it so they cannot evict it from the repository cache quota.
- `clang_tidy.yml` reuses this image, checks out with `fetch-depth: 2`, and passes `BASE=HEAD^1`
  to `cmake/tidy_incremental.cmake`. Configure the `debug` preset and build only
  `howdy_generated_headers`: tidy needs `compile_commands.json` and generated headers (version
  metadata and the Rust FFI header), not objects.
- `archlinux.yml` runs `howdy_cargo_deny` after the build; it needs network access for the
  advisory database, so it stays outside CTest.
- E2E tests are excluded. GitHub Actions runners do not expose the required container privileges.

## Local CI

Run from the repository root:

```sh
podman run --rm \
  -v "$PWD:/workspace:ro" \
  --tmpfs /workspace/build:rw,exec,notmpcopyup \
  -w /workspace \
  ghcr.io/nathawatc/howdy-next/ci-archlinux:latest bash -c '
    set -e
    cmake --preset release -DHOWDY_WARNINGS_AS_ERRORS=ON
    cmake --build --preset release --parallel "$(nproc)"
    ctest --preset release
  '
```

Run clang-tidy on code changed since `main`:

```sh
podman run --rm \
  -v "$PWD:/workspace:ro" \
  --tmpfs /workspace/build:rw,exec,notmpcopyup \
  -w /workspace \
  ghcr.io/nathawatc/howdy-next/ci-archlinux:latest bash -c '
    set -e
    cmake --preset debug
    cmake --build --preset debug --target howdy_generated_headers
    cmake -DBUILD_DIR=build/debug -DBASE="$(git merge-base HEAD main)" \
      -P cmake/tidy_incremental.cmake
  '
```

Rebuild the CI image when dependencies or the image recipe change. Image build and publishing
commands are maintainer-only and live in [`../README.md`](../README.md).
