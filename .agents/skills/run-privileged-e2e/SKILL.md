---
name: run-privileged-e2e
description: Build and run the privileged installed PAM/setuid E2E test suite via run0.
disable-model-invocation: true
---

# Run Privileged End-to-End Tests

Build artifacts as a normal user against the dedicated `/opt/howdy-installed-pam-e2e-local` prefix,
then execute only the privileged CTest suite under `run0`.

## Steps

1. **Configure and build as a normal user**:

   ```sh
   cmake --preset e2e
   cmake --build --preset e2e --parallel "$(nproc)"
   ```

2. **Run the privileged suite** with `HOWDY_E2E_USER` set to an existing non-root account (the
   suite also accepts a valid non-root `SUDO_USER` from privilege wrappers):

   ```sh
   run0 --setenv=HOWDY_E2E_USER="$USER" ctest --preset e2e
   ```

   The test temporarily installs below `/opt/howdy-installed-pam-e2e-local` and removes the
   installation and runtime staging on exit. On systems lacking `pam_start_confdir`, the tests exit
   77 (`SKIP`).

## Done Criteria

- `howdy-installed-e2e-artifacts` builds with `-DHOWDY_WARNINGS_AS_ERRORS=ON` and zero errors.
- `installed-pam-setuid-e2e` and `installed-pam-setuid-install-environment` pass (not skipped) with
  0 failures, and `/opt/howdy-installed-pam-e2e-local` is cleaned up afterward.
