# CI Images

> **For maintainers only.**
>
> Contributors should use the local CI runner commands in [`archlinux/AGENTS.md`](archlinux/AGENTS.md)
> and [`nix/AGENTS.md`](nix/AGENTS.md). Run all commands below from the repository root.

## Arch Linux

### Build Using Cache

```sh
podman build \
  -t ghcr.io/nathawatc/howdy-next/ci-archlinux:latest \
  -f ci/archlinux/Containerfile .
```

### Fresh Build

```sh
podman build --pull=always --no-cache \
  -t ghcr.io/nathawatc/howdy-next/ci-archlinux:latest \
  -f ci/archlinux/Containerfile .
```

### Push

```sh
podman push \
	--compression-format=zstd \
	--compression-level=20 \
	ghcr.io/nathawatc/howdy-next/ci-archlinux:latest
```

## Debian, Ubuntu, and Fedora

The images build OpenCV 5 (and yyjson or inih when the distribution's pkg-config version is below
the floor) into `/opt/howdy-deps` through `ci/common/build-deps.sh`. The first build is slow; later
builds reuse the cached layers. The tag follows `<release>-<revision>` and must match
`.github/workflows/distros.yml`.

### Build Using Cache

```sh
podman build \
  -t ghcr.io/nathawatc/howdy-next/ci-<distro>:<release>-<revision> \
  -f ci/<distro>/Containerfile .
```

`<distro>` is `debian`, `ubuntu`, or `fedora`, for example `ci-debian:13-1`.

### Fresh Build

```sh
podman build --pull=always --no-cache \
  -t ghcr.io/nathawatc/howdy-next/ci-<distro>:<release>-<revision> \
  -f ci/<distro>/Containerfile .
```

On a slow link the Fedora build can crawl on a distant mirror. Pin a fast one for a local build with
`--build-arg FEDORA_REPO_URL=<repo>`, for example
`'https://mirror.freedif.org/fedora/fedora/linux/development/$releasever/Everything/$basearch/os/'`.
It replaces the default repositories and keeps the GPG check. Leave it unset for published images.

### Push

```sh
podman push \
	--compression-format=zstd \
	--compression-level=20 \
	ghcr.io/nathawatc/howdy-next/ci-<distro>:<release>-<revision>
```

## Nix

The image tag follows `<nix-version>-<revision>` and must match `.github/workflows/nix.yml`.

### Build Using Cache

```sh
podman build \
  -t ghcr.io/nathawatc/howdy-next/ci-nix:<nix-version>-<revision> \
  -f ci/nix/Containerfile .
```

### Fresh Build

```sh
podman build --pull=always --no-cache \
  -t ghcr.io/nathawatc/howdy-next/ci-nix:<nix-version>-<revision> \
  -f ci/nix/Containerfile .
```

### Push

```sh
podman push \
	--compression-format=zstd \
	--compression-level=20 \
	ghcr.io/nathawatc/howdy-next/ci-nix:<nix-version>-<revision>
```

Authenticate with `podman login ghcr.io` before pushing if needed.

## Port Findings

Distribution differences found while building these images, recorded for the port work. None of
them is fixed by the CI change.

1. Debian 13 packages inih r59, but its `INIReader.pc` reports version 58, so the
   `INIReader>=59` floor in `CMakeLists.txt` fails. The Debian image builds inih r62 instead.
   Lowering the floor to 58 would let Debian use its own package.
2. Ubuntu's `/var/log` is `root:syslog 0775`, so `howdy snapshot` fails the group-writable
   ancestor check. `native-file-security` and `native-snapshot-writer` pin that verdict.
3. Debian's `dpkg-buildflags` passes `-D_FORTIFY_SOURCE=2`, which collides with the `=3` the
   project adds for Release builds. The warning is an error under `HOWDY_WARNINGS_AS_ERRORS`.
4. Debian's `dh_fixperms` strips setuid from `howdy-auth-helper` and widens the `/etc/howdy` and
   `config.ini` modes. Packaging needs overrides.
5. `ScopedMessageLocale` reads the process environment, but Debian and Ubuntu put `LANG` in the
   PAM environment through `pam_env`, so Thai prompts may not appear there.
6. Nothing handles SELinux on Fedora: gdm reaching the camera through `howdy-compare`, and the
   setuid helper creating `/run/howdy`.
7. `howdy download-models` writes to `/usr/share/howdy/models` at runtime. Debian policy and
   Fedora Atomic (read-only `/usr`) prefer `/var/lib/howdy`, which the Nix package already uses.
8. Distribution OpenBLAS is multi-threaded, and OpenCV linked against it exhausted the compare
   sandbox's 2.5 GiB `RLIMIT_AS` on a 16-core machine (Debian 13). Fixed in compare by pinning
   `OPENBLAS_NUM_THREADS` and `OMP_NUM_THREADS` (#13). The CI images build OpenCV with LAPACK
   against the distribution's OpenBLAS, and `native-compare-sandbox-workload` runs with the same
   pins; without them the model workload fails with OpenCV "Insufficient memory".
