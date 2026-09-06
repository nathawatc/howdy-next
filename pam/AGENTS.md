# PAM Module Knowledge Base

**Updated:** 2026-09-06

## Scope

C++ PAM authentication module for facial-recognition authentication on Linux
PAM-enabled services.

## Where to Look

| File                                          | Role                                                                                                                                                            |
| --------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `src/module/main.cpp`                         | PAM authentication entry point                                                                                                                                  |
| `src/module/unsupported_entrypoints.cpp`      | Unsupported PAM hooks returning `PAM_IGNORE`                                                                                                                    |
| `src/module/auth_flow.cpp`                    | Auth policy, readiness, status mapping, and coordinator composition                                                                                             |
| `src/runtime/runtime_session.cpp`             | Auth-helper staging, typed config load, and staged cleanup                                                                                                      |
| `src/runtime/auth_helper_process.cpp`         | Auth-helper high-level orchestration and public API                                                                                                             |
| `src/runtime/auth_helper_process/`            | Private implementation: spawn (`spawn.cpp`), bounded I/O and wait (`io.cpp`), lease transport/validation (`lease.cpp`), and internal contracts (`internal.hpp`) |
| `src/runtime/compare_process.cpp`             | Compare launch environment/argv, descriptor closure, timeout, and reap                                                                                          |
| `src/prompt/prompt_coordinator.cpp`           | Compare lifecycle, worker ownership, prompt race, and restoration                                                                                               |
| `src/prompt/native_prompt_conversation.cpp`   | Native TTY conversation, abort, and fail-closed restoration                                                                                                     |
| `src/prompt/observed_prompt_conversation.cpp` | Caller-thread secret-prompt observation and synchronous delegation                                                                                              |
| `src/prompt/conversation_response.cpp`        | Secure PAM conversation-response erasure and release                                                                                                            |
| `include/runtime/runtime_session.hpp`         | One-shot staged runtime/config boundary                                                                                                                         |
| `include/prompt/prompt_coordinator.hpp`       | Compare launch, worker, and prompt-race boundary                                                                                                                |
| `CMakeLists.txt`                              | PAM build and logical test-suite source lists                                                                                                                   |

## Security Invariants

- The caller thread owns every PAM operation: conversation installation,
  `pam_get_authtok()`, synchronous original-conversation delegation, and
  conversation restoration. The compare worker performs no PAM operation.
- The `PromptCoordinator` worker owns compare wait, cancellation, termination,
  and reap. One worker owns one child lifecycle. Inspect child status with
  `WIFEXITED` and `WIFSIGNALED`.
- Input mode permits one best-effort synthetic Enter attempt after the
  production conversation wrapper observes `PAM_PROMPT_ECHO_OFF`. Observation
  does not prove graphical callback readiness or focus. Enter may begin only
  while that secret-prompt generation remains active and before
  `pam_get_authtok()` return publication. State-mutex linearization decides the
  race; password return, generation close, or shutdown cancels a claimed action
  that has not started emission. Generation close leaves the one-shot
  opportunity pending for a later active generation; emission consumes it.
- Input Enter is one-shot and may be ineffective without keyboard focus. Manual
  completion remains available. Enter failure is logged through syslog without
  emitting a PAM notice for runtime Enter failure.
- Native mode requires `PAM_TTY` to be opened, foreground for the process group,
  and identifiable by at least one of stdin/stdout/stderr. Other descriptors may
  be redirected; graphical consumers with unrelated standard descriptors fail
  native eligibility.
- Normalize every long-lived Howdy-owned descriptor above `STDERR_FILENO` with
  transactional cleanup. Descriptor allocation is process-wide, so an opened
  descriptor may transiently occupy a closed standard-fd slot; final Howdy-owned
  descriptors must not retain those slots.
- Native and observed conversation restoration reports original-restored,
  static-fail-closed-installed, or unsafe. Non-original outcomes force
  `PAM_SYSTEM_ERR`; unsafe callback context is quarantined or otherwise retained
  fail closed for lifetime safety.
- `RuntimeSession` owns auth-helper staging, staged config/model paths, typed
  runtime-config loading, and cleanup. It is one-shot; repeated load attempts
  fail closed. Invalid staged paths or missing typed config fail closed.
- Preserve exact compare launch behavior: direct runtime has an empty
  environment; staged runtime exports only `HOWDY_USER_MODELS_DIR`; argv remains
  `howdy-compare --config <config-path> <username>`.
- Parse auth-helper output through `parse_auth_helper_output()` and shared keys
  in `protocol/auth_helper_protocol.hpp`. Reject malformed lines,
  duplicate/missing/unknown keys, read errors, output-limit hits, helper exit
  failures, and timeouts.
- Erase discarded PAM response strings through
  `secure_free_conversation_responses()`, including `explicit_bzero()` before
  release. Preserve PAM-owned authentication tokens as `const char *`; do not
  cast away constness or assume ownership.
- `auth_flow.cpp` owns service policy and maps RuntimeSession/
  PromptCoordinator results to PAM behavior. Invalid dependencies and
  unexpected errors map fail closed to `PAM_SYSTEM_ERR`; do not duplicate
  staging or child-lifecycle orchestration there.
- Do not restore conditional PAM test-mode behavior in production code. Test-only
  hooks remain local to test builds. Handle every conversation and syslog path.

## Test Organization

CTest logical suites may contain multiple translation units. Inspect
`pam/CMakeLists.txt` and sibling test sources; a driver is not the complete
suite. Shared headers used only by tests live under `tests/include/`.

### Auth Flow Logical Suite

- `tests/module/auth_flow_helpers_test.cpp` — suite driver and helper-policy
  tests.
- `tests/module/auth_flow_integration_test.cpp` — authentication orchestration.
- `tests/runtime/auth_helper_output_test.cpp` — auth-helper output/protocol
  behavior.
- `tests/runtime/process_wait_test.cpp` — process wait, timeout, and reap
  behavior.
- `tests/include/module/auth_flow_test_support.hpp` — shared auth-flow fixture.

### Native Prompt Logical Suite

- `tests/prompt/native_prompt_conversation_test.cpp` — driver, lifecycle, and
  restoration.
- `tests/prompt/native_prompt_terminal_test.cpp` — terminal eligibility.
- `tests/prompt/native_prompt_fd_test.cpp` — descriptor/setup transactionality.
- `tests/prompt/native_prompt_input_test.cpp` — PTY/input/EINTR/restore behavior.
- `tests/include/prompt/native_prompt_test_support.hpp` — shared native prompt
  fixture.

Prompt-coordinator test support is narrow, not one monolithic header:

- `tests/include/prompt/prompt_coordinator_test_access.hpp`
- `tests/include/prompt/prompt_coordinator_fake.hpp`
- `tests/include/prompt/prompt_coordinator_native_support.hpp`
- `tests/include/prompt/compare_spawn_test_support.hpp`

Generic process-test lifecycle support is
`tests/include/support/process_test_support.hpp`.

Retain runtime-session multi-source mapping:

- `tests/runtime/runtime_session_test.cpp` — core session suite driver.
- `tests/runtime/runtime_session_spawn_test.cpp` — helper spawn and process
  paths.
- `tests/runtime/runtime_session_deadline_test.cpp` — deadline and timeout
  paths.

The prompt-coordinator and runtime-session CTest suites likewise use their
focused sibling sources listed in `pam/CMakeLists.txt`.

## Production Cohesion

Do not fragment these cohesive security-sensitive state/process boundaries merely
for LOC. Child lifecycle, race, restoration, and ownership invariants are
simpler to audit while they remain together:

- `auth_helper_process` subsystem: remains one security-sensitive process/lifecycle
  subsystem split strictly across `src/runtime/auth_helper_process.cpp` (high-level
  orchestration and public API) and the private `src/runtime/auth_helper_process/`
  implementation directory (`spawn.cpp` for descriptor/spawn setup, `io.cpp` for
  bounded I/O, timeout, wait, and reap, `lease.cpp` for lease transport and
  validation, and `internal.hpp` for private implementation plumbing). Do not
  fragment this subsystem further merely for LOC; future changes must preserve
  process, descriptor-ownership, timeout, cleanup, protocol, and lease-validation
  invariants across the entire subsystem.
- `src/prompt/prompt_coordinator.cpp`
- `src/prompt/native_prompt_conversation.cpp`
