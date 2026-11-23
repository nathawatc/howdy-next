# PAM Test Guidelines

Parent: [`../AGENTS.md`](../AGENTS.md). Read the matching `src/<subsystem>/AGENTS.md` before
changing tests for a subsystem.

## Rules

- `pam/cmake/tests/*.cmake` defines logical CTest suites (several span multiple `.cpp` files).
- Place PAM test-only shared headers under `pam/tests/include/`; prefer focused fixtures over
  monolithic test-support headers.
- Preserve caller-thread vs worker-thread ownership in test doubles; never relax PAM threading rules
  to make tests pass.
- Cover child exit/signal/timeout/reap, descriptor cleanup, conversation restoration, malformed
  helper output, and fail-closed mapping where relevant.
