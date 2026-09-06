# PAM Module Guidelines

**Updated:** 2026-09-06

Read [`../../AGENTS.md`](../../AGENTS.md) first.

## Ownership

- `main.cpp`: `pam_sm_authenticate` ABI façade
- `main/unsupported_entrypoints.cpp`: unsupported PAM hooks returning `PAM_IGNORE`
- `production_entrypoint.cpp`: production dependency composition
- `entrypoint.cpp`: injectable entrypoint boundary
- `auth_flow.cpp`: service policy, readiness, runtime/prompt composition, PAM result mapping
- `auth_eligibility.cpp`, `pam_options.cpp`, `status_mapping.cpp`: focused policy helpers

## Rules

Keep ABI entrypoints thin. Do not expose a fake `main.hpp` solely for layout symmetry.
`auth_flow.cpp` owns authentication policy, not child lifecycle or staging internals.

Unexpected states and invalid dependencies map fail closed to `PAM_SYSTEM_ERR`. Preserve
unsupported-hook behavior and exported PAM symbols.
