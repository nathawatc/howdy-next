# CI Images

> **For maintainers only.**
>
> Contributors should use the local CI runner commands in [`archlinux/AGENTS.md`](archlinux/AGENTS.md)
> and [`nix/AGENTS.md`](nix/AGENTS.md). Run all commands below from the repository root.

## Arch Linux

### Build Using Cache

```sh
podman build \
  -t codeberg.org/nathawat/howdy-next/ci-archlinux:latest \
  -f ci/archlinux/Containerfile .
```

### Fresh Build

```sh
podman build --pull=always --no-cache \
  -t codeberg.org/nathawat/howdy-next/ci-archlinux:latest \
  -f ci/archlinux/Containerfile .
```

### Push

```sh
podman push \
	--compression-format=zstd \
	--compression-level=20 \
	codeberg.org/nathawat/howdy-next/ci-archlinux:latest
```

## Nix

The image tag follows `<nix-version>-<revision>` and must match `.forgejo/workflows/nix.yml`.

### Build Using Cache

```sh
podman build \
  -t codeberg.org/nathawat/howdy-next/ci-nix:<nix-version>-<revision> \
  -f ci/nix/Containerfile .
```

### Fresh Build

```sh
podman build --pull=always --no-cache \
  -t codeberg.org/nathawat/howdy-next/ci-nix:<nix-version>-<revision> \
  -f ci/nix/Containerfile .
```

### Push

```sh
podman push \
	--compression-format=zstd \
	--compression-level=20 \
	codeberg.org/nathawat/howdy-next/ci-nix:<nix-version>-<revision>
```

Authenticate with `podman login codeberg.org` before pushing if needed.
