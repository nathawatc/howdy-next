# Nix CI Guidelines

Parent: [`../../AGENTS.md`](../../AGENTS.md). Maintainer image build/push:
[`../README.md`](../README.md).

## Scope

Covers `ci/nix/`, `nix/`, `flake.nix`, `flake.lock`, and `.github/workflows/nix.yml`.

## Invariants

- Base `Containerfile` on the pinned `nixos/nix` version and tag published images
  `<nix-version>-<revision>` matching `.github/workflows/nix.yml`.
- Prebuild `ci-dependencies` and `ci-runtime`, never Howdy itself; run image build and
  `nix-store --gc` in a single layer.
- Keep `check-prebuilt.sh` as the single fail-fast guard for stale or incomplete CI images.
- Run workflow checks offline with `--no-update-lock-file`; evaluate both supported Linux systems
  (`x86_64-linux`, `aarch64-linux`), and build/test the Howdy package on `x86_64-linux` CI.
