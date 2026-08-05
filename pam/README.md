# Howdy PAM Module

## Requirements

The PAM module links against `INIReader` and `libevdev`. Install both development
packages before building.

Common package names are:

- Arch Linux: `libinih` and `libevdev`
- Debian: `libinih-dev` and `libevdev-dev`
- Fedora: `inih-devel` and `libevdev-devel`
- OpenSUSE: `inih` and `libevdev-devel`

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
