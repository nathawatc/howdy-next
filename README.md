# Howdy Next

Modern C++ reimplementation of Howdy facial authentication on Linux

> [!NOTE]
>
> Howdy Next is feature-complete and actively maintained. Development now
> focuses on bug fixes, security fixes, and platform/dependency compatibility.
> Low commit activity does not mean the project is abandoned.

## Packages

### Arch Linux (AUR)

> [!WARNING]
>
> Do not mix source installs with AUR package files.
> Use AUR packages to avoid mixed files under `/usr` and `/usr/local`.

Packages:

- `howdy-next`
- `howdy-next-git`

Example:

```sh
paru -S howdy-next
```

### NixOS

Use the included flake directly:

```sh
nix build
nix develop
```

See the [wiki](https://github.com/nathawatc/howdy-next/wiki/NixOS) for installation, setup, and PAM configuration.

## Build from Source

Dependencies:

```text
linux>=5.3 glibc>=2.36, cmake>=3.31, acl, pkgconf, gettext, libevdev, libinih>=59, opencv>=5.0.0, libcurl>=7.85.0, openssl, yyjson>=0.12.0, pam
```

The PAM compare requires `pidfd_open`, `eventfd`, and `ppoll`. Configure does not probe the kernel.

Build tools: GCC/Clang, GNU Make/Ninja, CMake 3.31+, Rust 1.85+ (cargo, rustc), Corrosion, cbindgen.
Packagers building without network access pass `-DHOWDY_CARGO_OFFLINE=ON` after fetching the
sources pinned in `Cargo.lock` (for example `cargo fetch --locked`).

Howdy uses OpenCV HighGUI for graphical preview. An OpenCV package may bring a
GUI backend such as Qt, depending on how the distribution builds OpenCV; Qt is
not a direct Howdy dependency.

### Release

For a distro-style system install, configure an appropriate prefix for your
system; `/usr` is an example:

```sh
cmake --preset release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build --preset release --parallel "$(nproc)"
ctest --preset release
```

PAM configuration uses `pam_howdy.so`. By default, Howdy installs it under
`${CMAKE_INSTALL_FULL_LIBDIR}/security`. If your distribution uses a different
PAM module directory, override it during configuration:

```sh
cmake --preset release \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DHOWDY_PAM_DIR=/path/to/pam/security
```

Install with the configured prefix:

```sh
sudo cmake --install build/release
```

For packaging, use an absolute `DESTDIR`. Howdy validates staged install paths and rejects unsafe
symlinks, hard links, special files, and non-copy `CMAKE_INSTALL_MODE` values. Existing `config.ini`
files are preserved.

Normal builds with `BUILD_TESTING=OFF` require only gettext's `msgfmt`. Translation
tests additionally require `xgettext`, `msgcmp`, and `msgattrib`. The `howdy-pot`
and `howdy-update-po` maintenance targets are available when `xgettext` and,
for updates, `msgmerge` are installed.

`cmake --install --prefix` is not supported. To use a different install prefix, reconfigure with `-DCMAKE_INSTALL_PREFIX=<path>`.

See [Contributing](CONTRIBUTING.md) for development, Debug builds, testing, and
contribution workflow.

### Setup

1. Identify camera path, preferably under `/dev/v4l/by-id/` or
   `/dev/v4l/by-path/`.
2. Run `sudo howdy config` and set `[video] device_path` to that path.
3. Run `sudo howdy add` to add face model.
4. Run `sudo howdy test` to test.

## CLI

See `howdy(1)` for the complete command and option reference.
See `pam_howdy(8)` for PAM configuration and workaround modes.

## Troubleshooting

Errors print to console. Check `journalctl -b -t pam_howdy` if auth fails quietly.

See [wiki](https://github.com/nathawatc/howdy-next/wiki/Troubleshooting) for
common issues.

> [!WARNING]
> Howdy is weaker than a password. Similar faces or photos may fool it.
> IR reduces spoofing because IR light is not reproduced by photos or LCD displays.
> Never use Howdy as the sole authentication method.

## License

Howdy Next is licensed under the GNU General Public License v3.0 or later
(`GPL-3.0-or-later`).

Revisions prior to the GPL relicensing commit were released under the MIT
License.

Third-party materials retain their respective licenses. See
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
