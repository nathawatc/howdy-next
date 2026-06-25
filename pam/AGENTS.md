# PAM Module Knowledge Base

**Updated:** 2026-06-25

## Scope

C++ PAM authentication module for facial-recognition auth on Linux PAM-enabled services.

## Where to Look

| File                                   | Role                                                      |
| -------------------------------------- | --------------------------------------------------------- |
| `src/main.cpp`                         | PAM entry point, compare process spawning, error handling |
| `src/auth_flow.cpp`                    | Auth helper spawning, output parsing, runtime file setup  |
| `src/enter_device.cpp`                 | Virtual keyboard helper that sends Enter                  |
| `src/status_mapping.cpp`               | Status and confirmation text mapping                      |
| `src/tests/status_mapping_test.cpp`    | Status mapping tests                                      |
| `src/tests/auth_flow_helpers_test.cpp` | Auth flow helper tests (output parsing, workaround, env)  |
| `main.hpp`                             | `ConfirmationType`, `Workaround`, `checkenv()`            |
| `optional_task.hpp`                    | Async task wrapper with timeout support                   |
| `meson.build`                          | PAM build configuration                                   |

## Conventions

- Do not change PAM auth flow without understanding session behavior.
- Never auto-terminate on success; PAM waits for user input.
- Use `WIFEXITED` and `WIFSIGNALED` to inspect child status.
- Keep every `conv_function` and syslog path fully handled.
- Treat runtime config load failures or missing typed config values as `PAM_SYSTEM_ERR`.
- Prefer pthread and standard C++ synchronization over `_thread`.
- Auth helper output parsed via `parse_auth_helper_output()`; rejects
  malformed lines (duplicate/missing/unknown keys). Protocol keys in
  `common/auth_helper_protocol.hpp` (`kConfigPathKey`, `kUserModelsDirKey`).
- Auth helper output reader is injectable for testing (`g_auth_helper_output_reader` under `HOWDY_PAM_TESTING`).
- `read_auth_helper_output()` fails closed on read errors, output limit
  hits, helper exit failures, and malformed protocol output.
