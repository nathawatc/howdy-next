# PAM Module Knowledge Base

**Updated:** 2026-06-11

## Scope

C++ PAM authentication module for facial-recognition auth on Linux PAM-enabled services.

## Where To Look

| File                                | Role                                                      |
| ----------------------------------- | --------------------------------------------------------- |
| `src/main.cpp`                      | PAM entry point, compare process spawning, error handling |
| `src/enter_device.cpp`              | Virtual keyboard helper that sends Enter                  |
| `src/status_mapping.cpp`            | Status and confirmation text mapping                      |
| `src/tests/status_mapping_test.cpp` | Status mapping tests                                      |
| `main.hpp`                          | `ConfirmationType`, `Workaround`, `checkenv()`            |
| `optional_task.hpp`                 | Async task wrapper with timeout support                   |
| `meson.build`                       | PAM build configuration                                   |

## Conventions

- Do not change PAM auth flow without understanding session behavior.
- Never auto-terminate on success; PAM waits for user input.
- Use `WIFEXITED` and `WIFSIGNALED` to inspect child status.
- Keep every `conv_function` and syslog path fully handled.
- Treat runtime config load failures or missing typed config values as `PAM_SYSTEM_ERR`.
- Prefer pthread and standard C++ synchronization over `_thread`.
