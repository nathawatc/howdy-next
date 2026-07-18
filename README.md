# Howdy Next

C++ rewrite of Howdy facial-recognition authentication on Linux

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

> [!NOTE]
>
> Atomic user-model writes require `renameat2(..., RENAME_EXCHANGE)` support
> from Linux kernel and filesystem containing configured user-model directory.
> Unsupported write configurations fail closed; clear/delete uses direct unlink.

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

`howdy [-U USER] [--plain] [-h] [-y] {command} [arguments...]`

| Command         | Description           |
| --------------- | --------------------- |
| add             | Add face model        |
| clear           | Remove all models     |
| config          | Edit config           |
| disable         | Enable/disable        |
| download-models | Download ONNX models  |
| list            | List models           |
| remove          | Remove specific model |
| set             | Edit config value     |
| snapshot        | Camera preview        |
| test            | Test camera           |
| version         | Show version          |

## Troubleshooting

Errors print to console. Check `journalctl -b -t pam_howdy` if auth fails quietly.

See [wiki](https://codeberg.org/nathawat/howdy-next/wiki/Troubleshooting) for
common issues.

> [!WARNING]
> Howdy weaker than password. Similar faces or photos may fool it.
> IR reduces spoofing. Invisible in photos and LCD displays.
> Never use as sole auth method.
