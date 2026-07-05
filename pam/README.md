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

## Build

```sh
meson setup build
ninja -C build
meson test -C build --print-errorlogs
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

Add `pam_howdy.so` to target PAM service under `/etc/pam.d/`.

Minimal example:

```pam
auth  sufficient  pam_howdy.so
```

## Options

`pam_howdy.so` accepts service-local password prompt workaround:

```pam
auth  sufficient  pam_howdy.so workaround=off
auth  sufficient  pam_howdy.so workaround=input
auth  sufficient  pam_howdy.so workaround=native
```

Default is `workaround=off`. `native` uses PAM conversation control to stop
Howdy's concurrent password prompt after face auth succeeds. `input` uses
`/dev/uinput` as fallback Enter key workaround. Option is PAM-local; it is not
read from `config.ini`.

PAM consumers that authenticate as regular user use installed
`howdy-auth-helper` setuid helper to prepare temp root-controlled runtime copies
of protected config and enrolled model before recognition.

If lock-screen auth fails before recognition starts, verify installed helper
mode with:

```sh
find /usr -path '*/howdy/howdy-auth-helper' -exec ls -l {} +
```

## PAM Ordering

PAM order depends on when consumer calls PAM.

Some consumers, like TTY login, sudo, polkit, and many graphical prompts, call
PAM before or while collecting a password. For those services, place Howdy
before password auth if face auth should run first or in parallel with the
password prompt.

Example:

```pam
auth  optional    pam_exec.so /usr/bin/linux-enable-ir-emitter run --config /etc/linux-enable-ir-emitter.toml
auth  sufficient  pam_howdy.so workaround=native
auth  sufficient  pam_unix.so try_first_pass nullok
auth  sufficient  pam_fprintd.so
```

Some lockers, including `waylock`, collect input before calling PAM. With
those lockers, Howdy cannot run before the locker's password entry screen
appears. Use service-specific order that validates already-submitted password
first, then falls through to Howdy and fingerprint auth.

For `waylock`, use `workaround=off`. `native` and `input` workarounds do not
make Howdy run before `waylock` calls PAM.

## Waylock

This stack keeps `waylock` order as:

```text
password -> Howdy -> fprintd
```

It also preserves `pam_faillock.so authsucc` on success and returns clear
failure state when all auth methods fail.

```pam
#%PAM-1.0

auth       required                    pam_faillock.so      preauth
# Optionally use requisite above if you do not want to prompt for the password
# on locked accounts.
-auth      [success=6 default=ignore]  pam_systemd_home.so
auth       optional                    pam_exec.so          /usr/bin/linux-enable-ir-emitter run --config /etc/linux-enable-ir-emitter.toml
auth       [success=4 default=ignore]  pam_unix.so          try_first_pass nullok
auth       [success=3 default=ignore]  pam_howdy.so         workaround=off
auth       [success=2 default=bad]     pam_fprintd.so       max-tries=1
auth       [default=die]               pam_faillock.so      authfail
auth       optional                    pam_permit.so
auth       required                    pam_env.so
auth       required                    pam_faillock.so      authsucc
# If you drop the above call to pam_faillock.so the lock will be done also
# on non-consecutive authentication failures.

-account   [success=1 default=ignore]  pam_systemd_home.so
account    required                    pam_unix.so
account    optional                    pam_permit.so
account    required                    pam_time.so

-password  [success=1 default=ignore]  pam_systemd_home.so
password   required                    pam_unix.so          try_first_pass nullok shadow
password   optional                    pam_permit.so

-session   optional                    pam_systemd_home.so
session    required                    pam_limits.so
session    required                    pam_unix.so
session    optional                    pam_permit.so
```

Auth control flow:

```text
pam_unix success    -> pam_env -> pam_faillock authsucc
pam_howdy success   -> pam_env -> pam_faillock authsucc
pam_fprintd success -> pam_env -> pam_faillock authsucc
all fail            -> pam_fprintd default=bad -> pam_faillock authfail
```

The final `pam_fprintd.so` line uses `default=bad` so stack records real auth
failure when password, Howdy, and fingerprint auth all fail. This gives
`waylock` clear failure result and resets prompt state.

`pam_fprintd.so` may still wait briefly for fingerprint input before return.
Sensor LED is not reliable signal that verification ended. Reduce `max-tries`
or remove `pam_fprintd.so` from `waylock` stack if faster retry feedback is
preferred.

## System-Auth

Avoid editing global `system-auth` unless change is intentional for every
service that includes it.

`system-auth` is shared by many PAM consumers, like login, sudo, polkit, display
managers, and lockers. Prefer service-specific PAM files when changing Howdy
ordering for one program.

If Howdy is added to a `pam_faillock` stack, do not use plain `sufficient`
lines without checking surrounding control flow. Successful auth must still
reach `pam_faillock.so authsucc`, and complete failure must be marked with real
failure state before `pam_faillock.so authfail`.

## Troubleshooting

If Howdy does not start from lock screen, check:

```sh
ls -ld /etc/howdy
ls -l /etc/howdy/config.ini
find /usr -path '*/howdy/howdy-auth-helper' -exec ls -l {} +
journalctl -b --no-pager | grep -i howdy
```

If `waylock` stays on password prompt after failed auth, check stack has real
failure path. In stack above, failure is recorded by:

```pam
auth       [success=2 default=bad]     pam_fprintd.so       max-tries=1
auth       [default=die]               pam_faillock.so      authfail
```

If `pam_fprintd.so` causes long delays after password and Howdy fail, reduce
`max-tries` or remove `pam_fprintd.so` from `waylock` service.
