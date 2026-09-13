# PAM Prompt Guidelines

**Updated:** 2026-09-13

Read [`../../AGENTS.md`](../../AGENTS.md) first.

## Ownership

- `prompt_coordinator.cpp`: compare/prompt race, worker ownership, cancellation, restoration, final
  result
- `native_prompt_conversation.cpp`: native TTY conversation, abort, terminal state, fail-closed
  restoration
- `observed_prompt_conversation.cpp`: caller-thread secret-prompt observation/delegation
- `conversation_response.cpp`: secure PAM response erasure/release
- `internal_fd.cpp`: safe Howdy-owned descriptor normalization

## Invariants

`pam_prompt_core` coordinates through explicit `PromptCoordinatorDependencies` callbacks. It must
not call production compare-process spawn, wait, cancel, or reap implementations directly;
production wiring belongs in `src/module/production_entrypoint.cpp`. Retain callback structs instead
of adding an OO abstraction layer.

The caller thread owns PAM conversation install/restore and `pam_get_authtok()`. The compare worker
performs no PAM operation and owns one child lifecycle.

Native prompt requires an eligible foreground TTY and safe descriptor identity. Long-lived
Howdy-owned descriptors must end above `STDERR_FILENO`.

Conversation restoration must report original-restored, fail-closed-installed, or unsafe.
Non-original outcomes force `PAM_SYSTEM_ERR`; unsafe callback context stays quarantined/retained for
lifetime safety.

Securely erase discarded PAM response strings before release. Do not cast away ownership of PAM
authentication tokens.

`prompt_coordinator.cpp` and `native_prompt_conversation.cpp` are cohesive state/lifecycle
boundaries; do not split them merely for LOC.
