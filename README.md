# Howdy Next

C++ rewrite of Howdy facial-recognition authentication on Linux

> [!NOTE]
>
> Howdy Next is feature-complete and actively maintained. Development now
> focuses on bug fixes, security fixes, and platform/dependency compatibility.
> Low commit activity does not mean the project is abandoned.

## Install from AUR

> [!WARNING]
>
> Do not mix source installs with AUR package files.
> Use AUR package to avoid mixed files under `/usr` and `/usr/local`.

Packages:

- `howdy-next`
- `howdy-next-git`

Example:

```sh
paru -S howdy-next
```

## Build from Source

Dependencies:

```text
glibc>=2.34, cmake>=3.31, acl, pkgconf, gettext, libevdev, libinih>=59, opencv>=5.0.0, qt6-base, libcurl>=7.85.0, openssl, yyjson>=0.12.0, pam
```

Build tools: GCC/Clang, GNU Make/Ninja, CMake 3.31+.

### Release

```sh
cmake --preset release
cmake --build --preset release --parallel "$(nproc)"
ctest --preset release
```

### Debug

```sh
cmake --preset debug
cmake --build --preset debug --parallel "$(nproc)"
ctest --preset debug
```

- Install with configured prefix: `sudo cmake --install build`

`cmake --install --prefix` is intentionally unsupported because runtime paths
are generated during configuration. Reconfigure with
`-DCMAKE_INSTALL_PREFIX=<path>` to change the install prefix.

See [Contributing](CONTRIBUTING.md) for workflow and rules.

### Setup

1. `sudo howdy add` to add face model.
2. `sudo howdy test` to test.
3. `sudo howdy config` to edit config.

## CLI

See `howdy(1)` for the complete command and option reference.
See `pam_howdy(8)` for PAM configuration and workaround modes.

## Troubleshooting

Errors print to console. Check `journalctl -b -t pam_howdy` if auth fails quietly.

See [wiki](https://codeberg.org/nathawat/howdy-next/wiki/Troubleshooting) for
common issues.

> [!WARNING]
> Howdy weaker than password. Similar faces or photos may fool it.
> IR reduces spoofing. Invisible in photos and LCD displays.
> Never use as sole auth method.

## License

Howdy Next is licensed under the GNU General Public License v3.0 or later
(`GPL-3.0-or-later`).

Revisions prior to the GPL relicensing commit were released under the MIT
License.

Third-party materials retain their respective licenses. See
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
