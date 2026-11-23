# Compare Guidelines

Parent: [`../../AGENTS.md`](../../AGENTS.md). Related:
[`../../../pam/src/runtime/AGENTS.md`](../../../pam/src/runtime/AGENTS.md).

## Boundaries

- `src/bin/compare.cpp` pins trust roots to UID 0 and calls `_exit` immediately after
  `RunCompareEntrypoint` flushes output and writes the verdict byte.
- Privilege dropping (`privileges.cpp`, `privileges/{operations,verification}.cpp`, private
  `privileges/internal.hpp`, shared/test seam `include/compare/privileges/internal.hpp`) is one
  auditable security subsystem: keep policy out of syscall adapters and keep the subsystem intact
  rather than splitting for LOC.

## Invariants

- Read config and user model strictly from inherited fds announced by
  `protocol/compare_descriptors.hpp` together with `protocol/compare_verdict.hpp`, with `user` as
  the sole positional argument.
- Call `TakeRuntimeDescriptors` before compare opens any file so announced numbers cannot alias
  compare's own fds. Abort on any missing or mismatched announcement or closed descriptor; take
  ownership of every announced fd and close each immediately after loading.
- Delegate fd type, owner, mode, and link checks to `LoadRuntimeConfigFromFd` and
  `LoadUserModelsFromFd` rather than duplicating them here.
- Preserve privilege-transition ordering, capability clearing, UID/GID/fs-ID checks, root-regain
  verification, rlimit/`PR_SET_NO_NEW_PRIVS`/seccomp-BPF ordering, and fatal fail-closed exits.
- Keep engine and capture orchestration tests ONNX-free by injecting inference.
