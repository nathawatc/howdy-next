# PAM Test Guidelines

Parent: [`../AGENTS.md`](../AGENTS.md). Read the matching `src/<subsystem>/AGENTS.md` before
changing tests for a subsystem.

## Rules

- Prefer focused fixtures over monolithic test-support headers under `pam/tests/include/`.
- Preserve caller-thread vs worker-thread ownership in test doubles; never relax PAM threading rules
  to make tests pass.
- Lock child exit/signal/timeout/reap races, descriptor cleanup, conversation restoration, malformed
  helper output, and fail-closed `PAM_SYSTEM_ERR` mappings with table-driven negative tests.
