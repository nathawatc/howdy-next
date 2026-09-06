# Howdy Test Guidelines

**Updated:** 2026-09-06

Read [`../AGENTS.md`](../AGENTS.md) first, then the production subsystem guide for code under test.

## Rules

- A CTest logical suite may span several `.cpp` files. Inspect `howdy/CMakeLists.txt` before moving
  or adding coverage.
- Keep test-only shared headers under `tests/include/<module>/`.
- Never add `tests/include` to production targets.
- Prefer injected filesystem, clock, capture, renderer, process, and inference dependencies over
  production side effects.
- Security-sensitive tests should cover success, malformed input, partial failure, rollback/cleanup,
  and uncertain-commit paths where relevant.
- Do not change production behavior merely to simplify a test.

Representative multi-source areas include CLI add/config/download-models, user-model codec/storage,
compare engine/privileges, auth helper, dispatcher/completion, and preview/snapshot.
