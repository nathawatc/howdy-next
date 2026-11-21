# Auth Helper Guidelines

Read [`../../AGENTS.md`](../../AGENTS.md) first.

## Ownership

- `command.cpp`: the setuid helper's `open` subcommand, its guards, and syslog diagnostics
- `runtime_descriptors.cpp` (library `howdy_runtime_descriptors`): the opener shared by `open` and
  a root PAM module

The helper's whole result is the one message of `protocol/runtime_descriptor_message.hpp` on
descriptor 3: the model state plus the config and, when the model is ready, the model descriptor.
When the config cannot be opened it sends nothing. PAM points the helper's stdio at `/dev/null`, so
guard failures and diagnostics also go to syslog. The flush and `_exit` in `src/bin/auth_helper.cpp`
follow `Run`, so static teardown never runs.

## Security Invariants

The helper trusts only the real UID: the argv user must be the caller's own passwd name, and it
validates the name and descriptor 3 (an `AF_UNIX` `SOCK_SEQPACKET` socket) before opening anything.
It ignores `HOWDY_CONFIG` and `HOWDY_USER_MODELS_DIR` while it runs setuid
(`config/runtime_paths.cpp`), so it only ever opens the installed files.

The opener opens the config by path with `O_NOFOLLOW` and then checks its ancestors, and opens the
model only after the model-store readiness walk and then checks the descriptor itself. Both
descriptors are read-only and close-on-exec. A missing model is a model state, not a failure. An
insecure model is reported as invalid storage with the config still sent, so PAM can decide
eligibility, and so is an empty model file: writers link a first model in whole at commit and never
truncate one, so an empty file is damaged storage, not an enrollment in progress. Do not weaken
these checks to simplify the helper.
