# PAM Package Guidelines

Read the [repository guidelines](../AGENTS.md) first.

## Subsystem Guides

- [`src/module/AGENTS.md`](src/module/AGENTS.md): PAM ABI and auth policy
- [`src/prompt/AGENTS.md`](src/prompt/AGENTS.md): prompt lifecycle and conversation safety
- [`src/runtime/AGENTS.md`](src/runtime/AGENTS.md): helper/compare process runtime
- [`src/runtime/auth_helper_process/AGENTS.md`](src/runtime/auth_helper_process/AGENTS.md): auth
  helper spawn, wait, and result message
- [`tests/AGENTS.md`](tests/AGENTS.md): PAM test organization

## Package Invariants

PAM authentication fails closed on invalid dependencies, a malformed or missing helper message,
rejected runtime descriptors, unsafe conversation restoration, unexpected process state, or config
failure. The auth helper's result is only its descriptor message; its exit status is never used. A
compare exit status the PAM application took (it reaped compare first) is replaced only by compare's
verdict byte, as `src/runtime/AGENTS.md` describes; without it the run fails closed.

The caller thread owns PAM operations. Worker threads must not call PAM APIs. Preserve secure
response erasure, descriptor ownership, child reap, timeout/cancellation behavior, and exact PAM
status mapping.

Production PAM module entrypoints live under `src/module/`; unsupported hooks return `PAM_IGNORE`.
Keep test-only behavior out of production code.

`pam/CMakeLists.txt` owns the module/install graph; `pam/cmake/tests/*.cmake` owns logical test
suites.
