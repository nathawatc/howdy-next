# PAM Module Guidelines

Parent: [`../../AGENTS.md`](../../AGENTS.md). Related: [`../prompt/AGENTS.md`](../prompt/AGENTS.md),
[`../runtime/AGENTS.md`](../runtime/AGENTS.md).

## Boundaries

- Keep `main.cpp` (`pam_sm_authenticate`) thin and omit a fake `main.hpp`. Unsupported PAM hooks
  live in `main/unsupported_entrypoints.cpp` (OBJECT library `pam_unsupported_entrypoints` so
  symbols stay exported) and return `PAM_IGNORE`.
- Keep `auth_flow.cpp` focused on authentication policy, free of child-lifecycle or
  descriptor-transport internals.
- Bind production prompt/runtime callbacks exclusively in `production_entrypoint.cpp`.

## Invariants

- Map unexpected states and invalid dependencies fail-closed to `PAM_SYSTEM_ERR`.
- Preserve exported PAM symbols (`pam/cmake/tests/check_module_exports.cmake`).
