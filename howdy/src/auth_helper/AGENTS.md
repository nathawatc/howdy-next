# Auth Helper Guidelines

Parent: [`../../AGENTS.md`](../../AGENTS.md). Related:
[`../storage/user_model_store/AGENTS.md`](../storage/user_model_store/AGENTS.md),
[`../../../pam/src/runtime/auth_helper_process/AGENTS.md`](../../../pam/src/runtime/auth_helper_process/AGENTS.md).

## Boundaries

- `runtime_descriptors.cpp` (`howdy_runtime_descriptors`) is the shared opener used both by the
  setuid helper's `open` subcommand (`command.cpp`) and by a root PAM module running in-process.
- `src/bin/auth_helper.cpp` flushes output and calls `_exit` immediately after `command::Run` so
  static teardown never runs.

## Security Invariants

- Trust only the real UID: `argv` user must match the `getuid()` passwd entry. Validate the username
  and descriptor 3 (`AF_UNIX` `SOCK_SEQPACKET`) before opening any file.
- While setuid, `config/runtime_paths.cpp` ignores `HOWDY_CONFIG` and `HOWDY_USER_MODELS_DIR` so
  only installed paths open.
- Open config with `O_NOFOLLOW` then check ancestors (`CheckSecureConfigFd`); open the user model
  through `UserModelStore::OpenRuntimeDescriptor`. Keep both descriptors read-only and `O_CLOEXEC`.
- Send strictly one `protocol/runtime_descriptor_message.hpp` message on fd 3 (send nothing when
  config fails to open). Report missing, insecure, or empty models as model states while still
  sending config so PAM decides eligibility; log guard failures and diagnostics to syslog because
  PAM redirects stdio to `/dev/null`.
