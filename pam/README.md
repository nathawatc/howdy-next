# Howdy PAM Module

## Requirements

PAM module needs `INIReader` and `libevdev`.

```text
Arch Linux - libinih libevdev
Debian     - libinih-dev libevdev-dev
Fedora     - inih-devel libevdev-devel
OpenSUSE   - inih libevdev-devel
```

Install `INIReader` package from your distro.

## Source Layout

```text
pam/
├── include/module/
├── include/prompt/
├── include/runtime/
├── src/module/
├── src/prompt/
├── src/runtime/
├── tests/
└── po/
```

## Development

The root CMake project builds the PAM module and its native tests.

See [CONTRIBUTING.md](../CONTRIBUTING.md) for build and contribution guidance.

## PAM Integration

See `pam_howdy(8)` for module syntax, options, and workaround modes.

See the [PAM Integration wiki page](https://codeberg.org/nathawat/howdy-next/wiki/PAM-Integration)
for service ordering, lock-screen setup, and troubleshooting.
