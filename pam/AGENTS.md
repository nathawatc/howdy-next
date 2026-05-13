# PAM Module Agent Knowledge

## OVERVIEW

C++ PAM authentication module providing facial recognition auth for Linux PAM-enabled services.

## WHERE TO LOOK

| File                               | Role                                                                      |
| ---------------------------------- | ------------------------------------------------------------------------- |
| `src/main.cc`                      | PAM entry point (`pam_sm_auth`), compare process spawning, error handling |
| `src/enter_device.cc`              | Virtual keyboard device that sends Enter keypress                         |
| `src/status_mapping.cc`            | Pure status/confirmation mapping used by PAM error handling               |
| `src/tests/status_mapping_test.cc` | Unit tests for PAM status mapping                                         |
| `main.hh`                          | CompareError enum, Workaround enum, `checkenv()` helper                   |
| `optional_task.hh`                 | Async task wrapper with timeout support                                   |
| `meson.build`                      | Build configuration                                                       |

## CONVENTIONS

- **Error codes**: CompareError enum (NO_FACE_MODEL=10, TIMEOUT_REACHED=11, ABORT=12, TOO_DARK=13, INVALID_DEVICE=14, RUBBERSTAMP=15)
- **PAM messaging**: Uses conv_function(PAM_ERROR_MSG, ...) or PAM_CONV_ERR for user communication
- **Signal handling**: WIFEXITED/WIFSIGNALED to inspect child process status
- **Workarounds**: Workaround enum (Off, Input=enter_device, Native=direct keyboard)
- **Threading**: pthread-based, std::mutex/std::condition_variable for sync
- **Config**: INIReader parses howdy config.ini (certainty, timeout, device_path)
- **i18n**: gettext via S() macro wrapper
- **Build**: Meson/Ninja, subproject for INIReader if not system-installed

## ANTI-PATTERNS

- **DO NOT auto-terminate on success**: PAM module must wait for user input after auth succeeds
- **DO NOT use PAM_SESSION_EXISTS**: unreliable, use other methods to detect session
- **Never skip error handling**: every syslog call and conv_function must have proper error cases
- **DO NOT use \_thread module**: use pthread or std::thread
