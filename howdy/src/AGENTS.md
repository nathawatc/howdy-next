# Howdy Source Guidelines

**Updated:** 2026-06-11

## Scope

Shared runtime code under `howdy/src/` and headers under `howdy/include/`.

## Conventions

- Prefer shared helpers for storage, config, readiness, model checks, and runtime staging.
- Do not reintroduce duplicated file-validation or config-loading logic in feature code.
- Runtime consumers should use typed `RuntimeConfig`, `VideoConfig`, and `FaceConfig` values instead of direct `ConfigReader` key lookups.
- `RuntimeConfigLoadResult::config` is optional; check status and `config.has_value()` before dereferencing.
- Keep runtime behavior fail-closed on validation or ownership errors.
- Use C++23 idioms when editing shared runtime code.
