# Howdy PAM Module

## Requirements

This module depends on `INIReader` and `libevdev`.
They can be installed with these packages:

```text
Arch Linux - libinih libevdev
Debian     - libinih-dev libevdev-dev
Fedora     - inih-devel libevdev-devel
OpenSUSE   - inih libevdev-devel
```

If your distribution doesn't provide `INIReader`,
it will be automatically pulled from git at the subproject's pinned version.

## Build

```sh
meson setup build
ninja -C build # or meson compile -C build
```

## Source Layout

```text
pam/
├── include/      # Public/private headers
├── src/          # PAM module sources
│   └── tests/    # Native unit tests
└── po/           # Translations
```

## Install

```sh
meson install -C build
```

Add the following line to your PAM configuration (/etc/pam.d/your-service):

```pam
auth  sufficient  pam_howdy.so
```

## Lock Screen Compatibility

Howdy Next keeps `/etc/howdy` locked down with `0750` on `/etc/howdy` and
`0640` on `config.ini`. PAM consumers that run authentication as the regular
user use the installed `howdy-auth-helper` setuid helper to prepare private
runtime copies of the protected config and enrolled model before recognition.

Do not make `/etc/howdy` or `config.ini` world-readable. If lock-screen auth
fails before recognition starts, verify that `$libdir/howdy/howdy-auth-helper`
is installed with the setuid bit.
