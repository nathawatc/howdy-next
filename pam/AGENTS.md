# PAM Package Guidelines

Parent: [`../AGENTS.md`](../AGENTS.md).

## Subsystems

- [`src/module/AGENTS.md`](src/module/AGENTS.md): PAM ABI, eligibility, auth flow, status mapping
- [`src/prompt/AGENTS.md`](src/prompt/AGENTS.md): prompt coordinator, TTY/observed conversations,
  uinput submitter
- [`src/runtime/AGENTS.md`](src/runtime/AGENTS.md): runtime session, compare and auth-helper process
  lifecycles
- [`tests/AGENTS.md`](tests/AGENTS.md): PAM test suites, threading fakes, and installed E2E

## Package Invariants

- Fail closed on invalid dependencies, malformed or missing helper messages, rejected runtime
  descriptors, unsafe conversation restoration, unexpected process state, or config errors.
- The caller thread owns every PAM API call; worker threads never call PAM APIs.
- Preserve secure response erasure, descriptor ownership, child reaping, timeout/cancellation
  semantics, and exact PAM status mapping.
- Pass owned descriptors as `ScopedFd` and child processes via spawn-time pidfds (`TrackedChild`,
  `HelperSpawn`) across internal helpers—never bare PIDs or unowned raw `int` fds outside thin
  syscall wrappers. Lock malformed helper messages, exit/reap races, and fail-closed
  `PAM_SYSTEM_ERR` mappings with table-driven negative tests.
- `pam_explicit_config_path` (`HOWDY_RUNTIME_CONFIG_EXPLICIT_PATH_ONLY`) ensures PAM never loads
  config from environment overrides.
- `pam/CMakeLists.txt` owns module and install targets; `pam/po/` owns gettext catalogs (`POTFILES`
  and `LINGUAS`, validated by `pam-translation-catalogs`).
