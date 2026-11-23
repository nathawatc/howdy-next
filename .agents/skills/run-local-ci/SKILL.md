---
name: run-local-ci
description: Run CI checks locally in Podman containers. Use when reproducing or validating Arch Linux CI builds, containerized clang-tidy, or offline Nix flake and package checks.
---

# Run Local CI

Run all commands from the repository root. Pick the branch matching the CI workflow you need to
reproduce.

## Steps

1. **Arch Linux release build and test** (mirrors `.github/workflows/archlinux.yml`):

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

2. **Arch Linux incremental `clang-tidy`** (mirrors `.github/workflows/clang_tidy.yml`):

   ```sh
   podman run --rm \
     -v "$PWD:/workspace:ro" \
     --tmpfs /workspace/build:rw,exec,notmpcopyup \
     -w /workspace \
     ghcr.io/nathawatc/howdy-next/ci-archlinux:latest bash -c '
       set -e
       cmake --preset debug
       cmake --build --preset debug --target howdy_version_metadata
       cmake -DBUILD_DIR=build/debug -DBASE="$(git merge-base HEAD main)" \
         -P cmake/tidy_incremental.cmake
     '
   ```

3. **Nix offline flake and package check** (mirrors `.github/workflows/nix.yml`; reads the pinned
   image tag directly from that workflow):

   ```sh
   NIX_CI_IMAGE=$(awk '/image:.*ci-nix:/ { print $2; exit }' .github/workflows/nix.yml)
   podman run --rm --network=none \
     -v "$PWD:/workspace:ro" -w /workspace \
     "$NIX_CI_IMAGE" bash -c '
       set -e
       bash ci/nix/check-prebuilt.sh
       nix flake check --offline --no-update-lock-file --no-build --all-systems
       nix build --offline --no-update-lock-file --no-link --max-jobs auto -L \
         .#checks.x86_64-linux.package
     '
   ```

## Done Criteria

- Every selected container command exits 0.
- If `ci/nix/check-prebuilt.sh` fails or Arch dependencies changed, report that the CI image needs
  rebuilding; image build and push commands are maintainer-only in `ci/README.md`.
