# Ubuntu CI Guidelines

Read the [repository guidelines](../../AGENTS.md) first.

## Scope

Covers `ci/ubuntu/`, the Ubuntu entries in `.github/workflows/distros.yml`, and the shared scripts in
`ci/common/`.

## Rules

- Keep the matrix image in `.github/workflows/distros.yml` aligned with
  `ghcr.io/nathawatc/howdy-next/ci-ubuntu:<release>-<revision>`, and the `FROM` line with
  `docker.io/library/ubuntu:<release>`.
- Install dependencies with apt. Build only what packages cannot satisfy, through
  `ci/common/build-deps.sh`; never build Howdy itself into the image.
- Keep the floors in `build-deps.sh` (OpenCV, yyjson, INIReader) aligned with the root
  `CMakeLists.txt`, and its OpenCV flags aligned with `nix/opencv5.nix`.
- Generate `th_TH.UTF-8` and `nl_NL.UTF-8` with `locale-gen`; `C.UTF-8` is built in.
- The workflow configures with `-DCMAKE_INSTALL_PREFIX=/usr` and `dpkg-buildflags` (CPPFLAGS folded
  into CXXFLAGS). Do not drop either:
  they are what make the install layout and compiler hardening match a real package build.
- The workflow sets `HOWDY_TEST_REQUIRE` so a missing locale, ACL support, seccomp, system PAM
  directory, or model file fails the run instead of skipping tests.
- E2E tests are excluded. GitHub Actions runners do not expose the required container privileges.

## Local CI

Run from the repository root:

```sh
podman run --rm \
  -v "$PWD:/workspace:ro" \
  --tmpfs /workspace/build:rw,exec,notmpcopyup \
  -w /workspace \
  ghcr.io/nathawatc/howdy-next/ci-ubuntu:26.04-1 bash -c '
    set -e
    eval "$(bash ci/common/distro-build-flags.sh dpkg)"
    cmake --preset release -DCMAKE_INSTALL_PREFIX=/usr -DHOWDY_WARNINGS_AS_ERRORS=ON
    cmake --build --preset release --parallel "$(nproc)"
    HOWDY_TEST_REQUIRE=locale-th,locale-nl,locale-c-utf8,acl,seccomp,pam-system-dir,models \
      ctest --preset release
  '
```

Rebuild the CI image when dependencies, `ci/common/build-deps.sh`, or the image recipe change. Image
build and publishing commands are maintainer-only and live in [`../README.md`](../README.md).
