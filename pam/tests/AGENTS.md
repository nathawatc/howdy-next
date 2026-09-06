# PAM Test Guidelines

**Updated:** 2026-09-06

Read [`../AGENTS.md`](../AGENTS.md) and the relevant `src/*/AGENTS.md` first.

## Rules

- CTest suites are defined in `pam/cmake/tests/*.cmake` and may span several translation units.
- Keep test-only shared headers under `tests/include/`.
- Preserve caller-thread vs worker-thread ownership in test doubles; do not make tests pass by
  allowing invalid PAM threading.
- Cover child exit/signal/timeout/reap, descriptor cleanup, conversation restoration, malformed
  helper output, and fail-closed mapping where relevant.
- Reuse focused fixtures instead of creating monolithic test-support headers.

Major multi-source suites include auth flow, native prompt conversation, prompt coordinator, and
runtime session.
