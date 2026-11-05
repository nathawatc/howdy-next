# Debian Packaging Rules

Scope: `debian/` source package for Debian 13. Root rules apply; this file adds packaging
invariants. CI image and workflow rules live in [`../ci/debian/AGENTS.md`](../ci/debian/AGENTS.md).

## Layout

- `rules` builds three things: yyjson (static), OpenCV 5 (shared, private), then Howdy.
- OpenCV and yyjson come from source components `opencv/` and `yyjson/`. They are pinned by
  version in `rules` and by checksum in `upstream/SHA256SUMS`. Never fetch during a build.
  `debian/rules get-orig-components` downloads, verifies, and unpacks them for a git checkout.
- Keep the OpenCV flags identical to `nix/opencv5.nix` and `ci/common/build-deps.sh`.

## Invariants

- OpenCV libraries live only in `/usr/lib/<multiarch>/howdy`. Ship no headers, `.pc`, CMake
  files, or `.so` development links, and no shlibs or ldconfig entry.
- The bundled libraries carry RUNPATH `$ORIGIN` so they find each other in the staging prefix
  and in the private directory. RUNPATH is not transitive, so the executables' own RUNPATH
  cannot do this. Do not replace it with `LD_LIBRARY_PATH`: the sandbox tests run children with
  a fixed environment.
- `debhelper` passes `-DCMAKE_SKIP_RPATH=ON`; `rules` turns it off for OpenCV and Howdy. With it
  on, `HOWDY_PRIVATE_LIBDIR` is silently ignored.
- `HOWDY_PRIVATE_LIBDIR` puts a RUNPATH on `howdy` and `howdy-compare` only. The setuid
  `howdy-auth-helper` and `pam_howdy.so` must never get an RPATH or RUNPATH.
- `override_dh_fixperms` must keep excluding `howdy-auth-helper`, `/etc/howdy`, and
  `/var/lib/howdy`: the helper is `4755 root:root`, `/etc/howdy` is `0750`, `config.ini` is `0640`.
- The `HOWDY_FLAGS` in `rules` must match the `debian-package` case in
  `tests/cmake/install_paths_matrix_test.cmake`. Change both together.
- The inih shim only applies when the installed `libinih-dev` is at least r59. Do not lower the
  `INIReader>=59` floor in `CMakeLists.txt` to avoid the shim.
- Models and enrolled data live under `/var/lib/howdy`. Purge removes them.
- The PAM profile ships with `Default: no`. Do not enable it from a maintainer script.

## Source Package

A binary build from a git checkout (`dpkg-buildpackage -b`) needs only the unpacked `opencv/`
and `yyjson/` components, which `debian/rules get-orig-components` provides. A source package
also needs the main orig tarball, which is the checkout without `debian/`:

```sh
git archive --prefix=howdy-next-3.4.2/ HEAD -- . ':(exclude)debian' \
  | xz > ../howdy-next_3.4.2.orig.tar.xz
```

Run `dpkg-source -b .` next to the three orig tarballs. Check binaries with
`lintian --fail-on error,warning ../*.changes`; build trees under `obj-*/` and `debian/` stay
out of git.

When `dpkg-buildpackage` is re-run with `-nc`, debhelper skips steps it already logged in
`debian/.debhelper`; delete that directory to force a rebuild. CMake also reads `LDFLAGS` only
when a build directory is first created, so change linker flags with a clean build.
