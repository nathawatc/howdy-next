# PAM MODULE KNOWLEDGE BASE

**Generated:** 2026-05-14

## OVERVIEW

C++ PAM authentication module for facial recognition auth on Linux PAM-enabled services.

## WHERE TO LOOK

| File                               | Role                                                      |
| ---------------------------------- | --------------------------------------------------------- |
| `src/main.cpp`                     | PAM entry point, compare process spawning, error handling |
| `src/enter_device.cpp`             | Virtual keyboard helper that sends Enter                  |
| `src/status_mapping.cpp`           | Status and confirmation text mapping                      |
| `src/tests/status_mapping_test.cpp` | Status mapping tests                                     |
| `main.hpp`                         | `CompareError`, `Workaround`, `checkenv()`                |
| `optional_task.hpp`                | Async task wrapper with timeout support                   |
| `meson.build`                      | PAM build configuration                                   |

## CONVENTIONS

- Do not change PAM auth flow without understanding session behavior.
- Never auto-terminate on success; PAM waits for user input.
- Use `WIFEXITED` and `WIFSIGNALED` to inspect child status.
- Keep every `conv_function` and syslog path fully handled.
- Prefer pthread and standard C++ synchronization over `_thread`.
