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
├── include/      # Public/private headers
├── src/          # PAM module sources
│   └── tests/    # Native unit tests
└── po/           # Translations
```

Add `pam_howdy.so` to target PAM service under `/etc/pam.d/`.

Minimal example:

```pam
auth  sufficient  pam_howdy.so
```

## PAM Integration

See [PAM Integration](https://codeberg.org/nathawat/howdy-next/wiki/PAM-Integration)
for module options, PAM ordering, lock-screen configuration, and troubleshooting.
