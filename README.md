# Howdy Next

A modernized fork of original Howdy, Facial recognition authentication for Linux.

## Building from Source

Dependencies: python3, pip, meson ≥0.64, ninja, libevdev

Debian/Ubuntu:

```text
sudo apt-get install python3 python3-pip cmake make build-essential libpam0g-dev libinih-dev
libevdev-dev python3-opencv python3-dev libopencv-dev
```

Build: `meson setup build && meson compile -C build` → Install: `meson install -C build`

## Setup

1. Run `sudo howdy add` to add a face model
2. Test with `sudo -i`

Edit config: `sudo howdy config`

## CLI

`howdy [-U user] [-y] command [argument]`

| Command  | Description           |
| -------- | --------------------- |
| add      | Add face model        |
| clear    | Remove all models     |
| config   | Edit config           |
| disable  | Enable/disable        |
| list     | List models           |
| remove   | Remove specific model |
| snapshot | Camera preview        |
| test     | Test camera           |
| version  | Show version          |

## Troubleshooting

Errors print to console. Check `/var/log/auth.log` if auth fails silently.

See [wiki](https://github.com/boltgolt/howdy/wiki/Common-issues) for common issues.

> [!WARNING]
> Howdy is NOT as secure as a password. Similar faces or photos can fool it.
> Keep in `/lib/security` read-only. **Never use as sole auth method.**
