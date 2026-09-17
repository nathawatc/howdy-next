# Nix CI Guidelines

**Updated:** 2026-09-14

Read the [repository guidelines](../../AGENTS.md) first.

## Scope

Covers `ci/nix/` and `.forgejo/workflows/nix.yml`.

## Rules

- Build from the pinned `nixos/nix` version used by `ci/nix/Containerfile`.
- Tag published CI images as `<nix-version>-<revision>`.
- Prebuild dependencies and CI runtime tools, but never prebuild Howdy itself.
- Keep `check-prebuilt.sh` as the single fail-fast guard for stale or incomplete images.
- Run workflow checks offline and without lock-file updates.
- Evaluate both supported Linux systems, but build and test the Howdy package only on x86_64 CI.
- Keep image construction and garbage collection in one layer.

## Local CI

Run from the repository root:

```sh
podman run --rm --network=none \
  -v "$PWD:/workspace:ro" -w /workspace \
  codeberg.org/nathawat/howdy-next/ci-nix:2.35.2-1 bash -c '
    set -e
    bash ci/nix/check-prebuilt.sh
    nix flake check --offline --no-update-lock-file --no-build --all-systems
    nix build --offline --no-update-lock-file --no-link --max-jobs 1 -L \
      .#checks.x86_64-linux.package
  '
```

Rebuild the CI image when the guard reports a changed dependency set or when runtime tools or the
image recipe change. Image build and publishing commands are maintainer-only and live in
[`../README.md`](../README.md).
