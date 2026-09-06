# PAM Package Guidelines

**Updated:** 2026-09-06

Read the [repository guidelines](../AGENTS.md) first.

## Subsystem Guides

- [`src/module/AGENTS.md`](src/module/AGENTS.md): PAM ABI and auth policy
- [`src/prompt/AGENTS.md`](src/prompt/AGENTS.md): prompt lifecycle and conversation safety
- [`src/runtime/AGENTS.md`](src/runtime/AGENTS.md): helper/compare process runtime
- [`tests/AGENTS.md`](tests/AGENTS.md): PAM test organization

## Package Invariants

PAM authentication fails closed on invalid dependencies, malformed helper output, unsafe
conversation restoration, unexpected process state, or staging/config failure.

The caller thread owns PAM operations. Worker threads must not call PAM APIs. Preserve secure
response erasure, descriptor ownership, child reap, timeout/cancellation behavior, and exact PAM
status mapping.

Production PAM module entrypoints live under `src/module/`; unsupported hooks return `PAM_IGNORE`.
Keep test-only behavior out of production code.

`pam/CMakeLists.txt` owns the module/install graph; `pam/cmake/tests/*.cmake` owns logical test
suites.
