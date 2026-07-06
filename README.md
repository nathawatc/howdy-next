# Howdy Next

C++ rewrite of Howdy facial-recognition authentication on Linux

## Install from AUR

> [!WARNING]
>
> Do not install Howdy Next manually with Meson.
> Use AUR package to avoid mixed files under `/usr` and `/usr/local`.

Packages:

- `howdy-next`
- `howdy-next-git`

Example:

```bash
paru -S howdy-next
```

## Build from Source

Dependencies:

```text
gcc, meson>=1.11.0, ninja, gettext, libevdev, libinih, opencv>=5.0.0, qt6-base, libcurl, openssl, yyjson>=0.12.0, pam
```

> [!NOTE]
>
> Atomic user-model writes require `renameat2(..., RENAME_EXCHANGE)` support
> from Linux kernel and filesystem containing configured user-model directory.
> Unsupported write configurations fail closed; clear/delete uses direct unlink.

- Build: `meson setup build && ninja -C build`
- Install: `meson install -C build`
- Test: `meson test -C build --print-errorlogs`

See [Contributing](CONTRIBUTING.md) for workflow and rules.

### Setup

1. Run `sudo howdy add` to add face model.
2. Test with `sudo howdy test`.
3. Edit config with `sudo howdy config`.

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

Errors print to console. Check `/var/log/auth.log` if auth fails quietly.

See [wiki](https://codeberg.org/nathawat/howdy-next/wiki/Troubleshooting) for
common issues.

> [!WARNING]
> Howdy is weaker than a password. Similar faces or photos may fool it.
> IR helps reduce spoofing. It is invisible in photos and LCD displays.
> Never use as sole auth method.
