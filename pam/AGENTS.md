# PAM Module Knowledge Base

**Updated:** 2026-08-02

## Scope

C++ PAM authentication module for facial-recognition auth on Linux PAM-enabled services.

## Where to Look

| File                                               | Role                                                                            |
| -------------------------------------------------- | ------------------------------------------------------------------------------- |
| `src/module/main.cpp`                              | PAM authentication entry point                                                  |
| `src/module/unsupported_entrypoints.cpp`           | Unsupported PAM hooks returning `PAM_IGNORE`                                    |
| `src/module/auth_flow.cpp`                         | Auth policy, readiness, status mapping, coordinator                             |
| `src/runtime/runtime_session.cpp`                  | Auth-helper staging, typed config load, staged cleanup                          |
| `src/prompt/prompt_coordinator.cpp`                | Compare lifecycle and prompt coordination                                       |
| `include/runtime/runtime_session.hpp`              | One-shot staged runtime/config boundary                                         |
| `include/prompt/prompt_coordinator.hpp`            | Compare launch and prompt-race boundary                                         |
| `src/prompt/enter_device.cpp`                      | Virtual keyboard helper that sends Enter                                        |
| `src/prompt/conversation_response.cpp`             | Secure PAM conversation-response erasure and release                            |
| `src/prompt/observed_prompt_conversation.cpp`      | Caller-thread secret-prompt observation and synchronous conversation delegation |
| `src/prompt/native_prompt_conversation.cpp`        | Native TTY conversation, abort, and fail-closed restoration                     |
| `src/module/status_mapping.cpp`                    | Status and confirmation text mapping                                            |
| `tests/runtime/runtime_session_test.cpp`           | Core runtime-session tests and split-suite entry point                          |
| `tests/runtime/runtime_session_spawn_test.cpp`     | Helper spawn and process-path tests                                             |
| `tests/runtime/runtime_session_deadline_test.cpp`  | Helper deadline and timeout tests                                               |
| `tests/prompt/prompt_coordinator_test.cpp`         | Core coordinator tests and split-suite entry point                              |
| `tests/prompt/prompt_coordinator_modes_test.cpp`   | Native/input workaround mode tests                                              |
| `tests/prompt/prompt_coordinator_adapter_test.cpp` | Production adapter tests                                                        |
| `tests/module/auth_flow_helpers_test.cpp`          | Auth-flow helpers and policy-adjacent tests                                     |
| `include/module/auth_eligibility.hpp`              | Typed eligibility conditions, policy, and injected runtime probes               |
| `include/module/entrypoint.hpp`                    | Generic PAM ABI adapter and exception boundary                                  |
| `include/module/production_entrypoint.hpp`         | Production authentication callback wiring                                       |
| `include/module/pam_options.hpp`                   | Typed PAM invocation arguments and workaround options                           |
| `include/prompt/workaround.hpp`                    | Prompt workaround mode and prompt policy helper                                 |
| `include/runtime/message_locale.hpp`               | Thread-local message and character locale guard                                 |
| `CMakeLists.txt`                                   | PAM build configuration                                                         |

## Conventions

- RuntimeSession owns auth-helper staging, staged config/model paths, typed
  runtime config loading, and cleanup. It is one-shot; repeated load attempts
  fail closed.
- Caller thread owns every PAM operation, including conversation install,
  `pam_get_authtok()`, synchronous original-conversation delegation, and
  conversation restoration. Compare worker performs no PAM operation.
- PromptCoordinator worker owns compare wait, cancellation, termination, and
  reap. One worker owns one child lifecycle.
- Input mode permits one best-effort synthetic Enter attempt after production
  conversation wrapper observes `PAM_PROMPT_ECHO_OFF`. This observation does not
  prove graphical callback readiness or focus. Enter may begin only while that
  secret-prompt generation remains active and before `pam_get_authtok()` return
  publication. Whichever publication wins state-mutex linearization decides the
  race; password return, generation close, or shutdown cancels a claimed action
  that has not started emission. Generation close keeps the one-shot opportunity
  pending so compare worker may claim a later active secret-prompt generation;
  emission permanently consumes it.
- Input Enter is one-shot and may be ineffective when target prompt lacks
  keyboard focus. Manual password completion remains available. Enter failure is
  logged immediately through syslog; no PAM notice is emitted for runtime Enter
  failure.
- Native mode requires opened `PAM_TTY` to be foreground terminal for process
  group and requires stdin, stdout, or stderr to identify same terminal. Other
  descriptors may be redirected; graphical consumers with unrelated stdio fail
  native eligibility.
- Normalize every long-lived Howdy-owned descriptor above `STDERR_FILENO` with
  transactional cleanup. Linux descriptor allocation remains process-wide, so a
  newly opened descriptor may transiently occupy a closed standard-fd slot before
  normalization; final Howdy-owned descriptors never retain those slots.
- Native restoration reports original-restored, static-fail-closed-installed,
  or unsafe. Non-original outcomes force `PAM_SYSTEM_ERR`; unsafe callback
  context becomes permanently fail closed and remains lifetime-safe.
- auth_flow.cpp owns service policy and maps RuntimeSession /
  PromptCoordinator results to PAM behavior. Keep it free of duplicated
  staging and child-lifecycle orchestration.
- Preserve exact compare launch behavior:
  - direct runtime has an empty environment;
  - staged runtime exports only `HOWDY_USER_MODELS_DIR`;
  - compare argv remains `howdy-compare --config <config-path> <username>`.
- PAM failures must remain fail-closed. Invalid runtime-session or
  prompt-coordinator dependencies map to `PAM_SYSTEM_ERR`.
- Do not restore conditional PAM test-mode behavior in production code.
  Test-only hooks remain local to test builds.
- Use `WIFEXITED` and `WIFSIGNALED` to inspect child status.
- Keep every `conv_function` and syslog path fully handled.
- Erase discarded PAM response strings through `secure_free_conversation_responses()`;
  it uses `explicit_bzero()` before release.
- Preserve PAM-owned authentication tokens as `const char *`; do not cast away
  constness or assume ownership of memory returned by PAM.
- Treat runtime config load failures or missing typed config values as `PAM_SYSTEM_ERR`.
- Prefer pthread and standard C++ synchronization over `_thread`.
- Auth helper output parsed via `parse_auth_helper_output()`; rejects malformed
  lines (duplicate/missing/unknown keys). Protocol keys in
  `protocol/auth_helper_protocol.hpp` (`kConfigPathKey`, `kUserModelsDirKey`).
- Auth helper process dependencies are passed explicitly through
  `auth_helper_process::Operations`; keep test state local to each test.
- `read_auth_helper_output()` fails closed on read errors, output limit hits,
  helper exit failures, and malformed protocol output.
