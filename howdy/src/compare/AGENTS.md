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
  the sole positional argument; never accept `--help`/`-h` when any compare protocol environment
  variable is present.
- Call `TakeRuntimeDescriptors` before compare opens any file so announced numbers cannot alias
  compare's own fds. Abort on any missing or mismatched announcement or closed descriptor; take
  ownership of every announced fd and close each immediately after loading.
- Delegate fd type, owner, mode, and link checks to `LoadRuntimeConfigFromFd` and
  `LoadUserModelsFromFd` rather than duplicating them here.
- Preserve privilege-transition ordering, capability clearing, UID/GID/fs-ID checks, root-regain
  verification, and fatal fail-closed exits. Keep the hardening order: `DisableCompareTracing`
  (clear `PR_SET_DUMPABLE`, then reject a tracer on any thread) before argument parsing or config
  loading; `ApplyCompareSandbox` (`PR_SET_NO_NEW_PRIVS`, then rlimits) before model loading;
  `DropComparePrivileges`, which clears dumpable again after any credential change; then
  seccomp-BPF.
- `DisableCompareTracing` requires a readable `/proc/self/task` and fails closed without it; its
  best-effort limitations documented next to the code are a settled contract. The entrypoint runs it
  again after `DropComparePrivileges`, before seccomp.
- Install seccomp right after the privilege drop and before engine construction, warm-up, or any
  extra thread, so `WarmUpRecognizer` runs inside the filter.
- Seccomp is best-effort only for `CompareSeccompStatus::kUnsupported` (kernel without seccomp
  filter mode) and `kUnsupportedArchitecture` (build without a filter for the target): warn and
  continue. Filter construction/installation failures and missing injected hooks abort. Tests cover
  the first against emulated `seccomp(2)` failures and the second by building the sandbox with
  `HOWDY_SECCOMP_UNSUPPORTED_ARCH`.
- Keep engine and capture orchestration tests ONNX-free by injecting inference.
