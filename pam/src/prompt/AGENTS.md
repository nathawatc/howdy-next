# PAM Prompt Guidelines

Parent: [`../../AGENTS.md`](../../AGENTS.md). Related: [`../module/AGENTS.md`](../module/AGENTS.md).

## Boundaries

- `pam_prompt_core` coordinates exclusively through `PromptCoordinatorOperations` callbacks and never
  calls production compare spawn/wait/cancel/reap implementations directly (production wiring lives
  in `src/module/production_entrypoint.cpp`).
- Keep `prompt_coordinator.cpp` and `native_prompt_conversation.cpp` cohesive rather than splitting
  them for LOC.

## Invariants

- The caller thread owns PAM conversation install/restore and `pam_get_authtok()`; the compare
  worker performs zero PAM calls and owns one child lifecycle.
- `PromptCoordinator` owns the child's pidfd and verdict socket returned by spawn until the run
  ends; wait and cancel only borrow them. If a failing or throwing spawn still returned a child with
  a pidfd, stop and reap it through that pidfd; without a pidfd, leave cleanup to spawn.
- Native prompt requires an eligible foreground TTY and safe fd identity; normalize long-lived
  Howdy-owned descriptors above `STDERR_FILENO` (`internal_fd.cpp`).
- Conversation restoration reports original-restored, fail-closed-installed, or unsafe: non-original
  outcomes force `PAM_SYSTEM_ERR`, and unsafe callback contexts remain quarantined for lifetime
  safety.
- Securely erase discarded PAM response strings (`conversation_response.cpp`) before release; never
  cast away ownership of PAM authentication tokens.
