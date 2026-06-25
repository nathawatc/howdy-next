# Howdy Next

Security-focused C++ rewrite of Howdy for facial-recognition authentication on Linux

## Install from AUR

> [!WARNING]
>
> Do not install Howdy Next manually with Meson.
> Use the AUR package to avoid mixed files under `/usr` and `/usr/local`.

Packages:

- `howdy-next`
- `howdy-next-git`

Example with an AUR helper:

```bash
paru -S howdy-next
```

## Building from Source

Dependencies:

```text
meson, ninja, clang-tidy, gettext, libevdev, libinih, libopencv, libcurl, openssl, nlohmann-json, pam
```

- Build: `meson setup build && ninja -C build`
- Install: `meson install -C build`
- Verify: `run-clang-tidy -p build`

### Setup

1. Run `sudo howdy add` to add a face model
2. Test with `sudo howdy test`

Edit config: `sudo howdy config`

## CLI

`howdy [-U user] [-y] command [argument]`

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

Errors print to console. Check `/var/log/auth.log` if auth fails silently.

See [wiki](https://codeberg.org/nathawat/howdy-next/wiki/Troubleshooting) for common issues.

> [!WARNING]
> Howdy is less secure than a password. Similar faces or photos may fool it.
> IR can help reduce spoofing. It is invisible in photos and LCD displays.
> **Never use as the sole auth method.**
