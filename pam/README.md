# Howdy PAM Module

## Requirements

The PAM module depends on `INIReader` and `libevdev`.

```text
Arch Linux - libinih libevdev
Debian     - libinih-dev libevdev-dev
Fedora     - inih-devel libevdev-devel
OpenSUSE   - inih libevdev-devel
```

Install the `INIReader` package from your distribution.

## Build

```sh
meson setup build
meson compile -C build
```

`ninja -C build` may also be used instead of `meson compile -C build`.

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

Add `pam_howdy.so` to the target PAM service under `/etc/pam.d/`.

Minimal example:

```pam
auth  sufficient  pam_howdy.so
```

## Options

`pam_howdy.so` accepts a service-local password prompt workaround option:

```pam
auth  sufficient  pam_howdy.so workaround=off
auth  sufficient  pam_howdy.so workaround=input
auth  sufficient  pam_howdy.so workaround=native
```

The default is `workaround=off`. `native` uses PAM conversation control to stop
Howdy's concurrent password prompt after face authentication succeeds, while
`input` uses `/dev/uinput` as a fallback-style Enter key workaround. This option
is intentionally PAM-local; it is not read from `config.ini`.

PAM consumers that run authentication as the regular user use the installed
`howdy-auth-helper` setuid helper to prepare temporary root-controlled runtime
copies of the protected config and enrolled model before recognition.

If lock-screen authentication fails before recognition starts, verify that the
helper is installed with the setuid bit:

```sh
ls -l $libdir/howdy/howdy-auth-helper
```

## PAM Ordering

PAM ordering depends on when the consumer calls PAM.

Some consumers, such as TTY login, sudo, polkit, and many graphical prompts,
call PAM before or while collecting a password. For those services, place Howdy
before password authentication if face authentication should run first or run in
parallel with the password prompt.

Example:

```pam
auth  optional    pam_exec.so /usr/bin/linux-enable-ir-emitter run --config /etc/linux-enable-ir-emitter.toml
auth  sufficient  pam_howdy.so workaround=native
auth  sufficient  pam_unix.so try_first_pass nullok
auth  sufficient  pam_fprintd.so
```

Some lockers, including `waylock`, collect input before calling PAM. With those
lockers, Howdy cannot run before the locker's password entry screen appears.
Use a service-specific order that validates the already-submitted password
first, then falls through to Howdy and fingerprint authentication.

For `waylock`, use `workaround=off`. The `native` and `input` prompt workarounds
do not make Howdy run before `waylock` calls PAM.

## Waylock

The following stack keeps the `waylock` order as:

```text
password -> Howdy -> fprintd
```

It also preserves `pam_faillock.so authsucc` on successful authentication and
returns a clear failure state when all authentication methods fail.

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

The auth control flow is:

```text
pam_unix success    -> pam_env -> pam_faillock authsucc
pam_howdy success   -> pam_env -> pam_faillock authsucc
pam_fprintd success -> pam_env -> pam_faillock authsucc
all fail            -> pam_fprintd default=bad -> pam_faillock authfail
```

The final `pam_fprintd.so` line uses `default=bad` so the stack records a real
authentication failure when password, Howdy, and fingerprint authentication all
fail. This lets `waylock` receive a clear failure result and reset the prompt
state.

`pam_fprintd.so` may still wait briefly for fingerprint input before returning.
The fingerprint sensor LED is not a reliable indicator that the verification
attempt has ended. Reduce `max-tries` or remove `pam_fprintd.so` from the
`waylock` stack if faster password retry feedback is preferred.

## System-Auth

Avoid editing global `system-auth` unless the change is intentional for every
service that includes it.

`system-auth` is shared by many PAM consumers, such as login, sudo, polkit,
display managers, and lockers. Prefer service-specific PAM files when changing
Howdy ordering for one program.

If Howdy is added to a `pam_faillock` stack, do not use plain `sufficient` lines
without checking the surrounding control flow. Successful authentication must
still reach `pam_faillock.so authsucc`, and complete failure must be marked with
a real failure state before `pam_faillock.so authfail`.

## Troubleshooting

If Howdy does not start from a lock screen, check:

```sh
ls -ld /etc/howdy
ls -l /etc/howdy/config.ini
ls -l $libdir/howdy/howdy-auth-helper
journalctl -b --no-pager | grep -i howdy
```

If `waylock` remains on the password prompt after failed authentication, check
that the PAM stack has a real failure path. In the recommended stack above,
failure is recorded by:

```pam
auth       [success=2 default=bad]     pam_fprintd.so       max-tries=1
auth       [default=die]               pam_faillock.so      authfail
```

If `pam_fprintd.so` causes long delays after password and Howdy fail, either
reduce `max-tries` or remove `pam_fprintd.so` from the `waylock` service.
