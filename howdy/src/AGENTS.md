# Howdy Source Guidelines

**Updated:** 2026-06-11

## Scope

Shared runtime code under `howdy/src/` and headers under `howdy/include/`.

## Recent Refactors

- `refactor(storage): harden user model storage operations (#16)`
- `refactor(storage): centralize user model readiness checks`
- `refactor(auth-helper): reuse user model readiness checks`
- `refactor(config): centralize edited config installation (#17)`
- `docs(agents): suppress clang-tidy ignored report`
- `build(release): bump cpp_std to c++23`
- `refactor(style): modernize C++ idioms for C++23`
- `refactor(auth-helper): split runtime auth file staging (#18)`
- `refactor(models): centralize OpenCV model file checks`
- `refactor(cli): inject download model deps`
- `style: convert to tabs indentation`
- `refactor(config): centralize runtime schema validation (#19)`
- `refactor(config): centralize runtime config loading (#20)`

## Conventions

- Prefer shared helpers for storage, config, readiness, model checks, and runtime staging.
- Do not reintroduce duplicated file-validation or config-loading logic in feature code.
- Keep runtime behavior fail-closed on validation or ownership errors.
- Use C++23 idioms when editing shared runtime code.
