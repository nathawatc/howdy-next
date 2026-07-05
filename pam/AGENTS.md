# PAM Module Knowledge Base

**Updated:** 2026-07-05

## Scope

C++ PAM authentication module for facial-recognition auth on Linux PAM-enabled services.

## Where to Look

| File | Role |
| --- | --- |
| `src/main.cpp` | PAM module entry points |
| `src/auth_flow.cpp` | Auth policy, readiness, status mapping, coordinator |
| `src/runtime_session.cpp` | Auth-helper staging, typed config load, staged cleanup |
| `src/prompt_coordinator.cpp` | Compare lifecycle and prompt coordination |
| `include/runtime_session.hpp` | One-shot staged runtime/config boundary |
| `include/prompt_coordinator.hpp` | Compare launch and prompt-race boundary |
| `src/enter_device.cpp` | Virtual keyboard helper that sends Enter |
| `src/status_mapping.cpp` | Status and confirmation text mapping |
| `src/tests/runtime_session_test.cpp` | Runtime staging, config, cleanup, one-shot tests |
| `src/tests/prompt_coordinator_test.cpp` | Prompt, compare-process, cleanup, one-shot tests |
| `src/tests/auth_flow_helpers_test.cpp` | Auth-flow helpers and policy-adjacent tests |
| `main.hpp` | `ConfirmationType`, `Workaround` (`off`, `input`, `native`), `checkenv()` |
| `optional_task.hpp` | Async task wrapper with timeout support |
| `meson.build` | PAM build configuration |

## Conventions

- RuntimeSession owns auth-helper staging, staged config/model paths, typed
  runtime config loading, and cleanup. It is one-shot; repeated load attempts
  fail closed.
- PromptCoordinator owns compare process lifecycle: spawn, wait, termination,
  and reap. It also owns native/input prompt coordination and cleanup.
- auth_flow.cpp owns service policy and maps RuntimeSession /
  PromptCoordinator results to PAM behavior. Keep it free of duplicated
  staging and child-lifecycle orchestration.
- Preserve exact compare launch behavior:
  - direct runtime has an empty environment;
  - staged runtime exports only `HOWDY_USER_MODELS_DIR`;
  - compare argv remains `howdy-compare --config <config-path> <username>`.
- PAM failures must remain fail-closed. Invalid runtime-session or
  prompt-coordinator dependencies map to `PAM_SYSTEM_ERR`.
- Do not restore `HOWDY_PAM_TESTING` behavior in production code.
  Test-only hooks remain local to test builds.
- Never auto-terminate on success; PAM waits for user input.
- Use `WIFEXITED` and `WIFSIGNALED` to inspect child status.
- Keep every `conv_function` and syslog path fully handled.
- Treat runtime config load failures or missing typed config values as `PAM_SYSTEM_ERR`.
- Prefer pthread and standard C++ synchronization over `_thread`.
- Auth helper output parsed via `parse_auth_helper_output()`; rejects malformed
  lines (duplicate/missing/unknown keys). Protocol keys in
  `common/auth_helper_protocol.hpp` (`kConfigPathKey`, `kUserModelsDirKey`).
- Auth helper output reader is injectable for testing
  (`g_auth_helper_output_reader` under `HOWDY_PAM_TESTING`).
- `read_auth_helper_output()` fails closed on read errors, output limit hits,
  helper exit failures, and malformed protocol output.
