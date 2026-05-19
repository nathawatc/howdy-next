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
user use the installed `howdy-auth-helper` setuid helper to prepare temporary
root-controlled runtime copies of the protected config and enrolled model before
recognition.

Do not make `/etc/howdy` or `config.ini` world-readable. If lock-screen auth
fails before recognition starts, verify that `$libdir/howdy/howdy-auth-helper`
is installed with the setuid bit.

Some lockers, including `waylock`, collect input before calling PAM. With those
lockers, Howdy cannot run before the locker's password entry screen appears.
Use a service-specific PAM order that validates the already-submitted password
first, then falls through to Howdy and fingerprint authentication:

```pam
auth  optional     pam_exec.so /usr/bin/linux-enable-ir-emitter run --config /etc/linux-enable-ir-emitter.toml
auth  sufficient   pam_unix.so try_first_pass nullok
auth  sufficient   pam_howdy.so
auth  sufficient   pam_fprintd.so
```

For TTY, sudo, and PAM consumers that call PAM before collecting a password,
keep `pam_howdy.so` before `pam_unix.so` if face authentication should run
first.
