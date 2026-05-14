# Howdy Next

A modernized fork of original Howdy, Facial recognition authentication for Linux.

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

See [wiki](https://github.com/boltgolt/howdy/wiki/Common-issues) for common issues.

> [!WARNING]
> Howdy is less secure than a password. Similar faces or photos may fool it.
> IR can help reduce spoofing. It is invisible in photos and LCD displays.
> **Never use as the sole auth method.**
