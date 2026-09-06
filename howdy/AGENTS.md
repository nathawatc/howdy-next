# Howdy Package Guidelines

**Updated:** 2026-09-06

Read the [repository guidelines](../AGENTS.md) first.

## Scope

Covers `howdy/include`, `howdy/src`, `howdy/tests`, package resources, and Howdy-specific CMake
targets.

## Subsystem Guides

- [`src/app/AGENTS.md`](src/app/AGENTS.md): dispatcher, command catalog, completion
- [`src/auth_helper/AGENTS.md`](src/auth_helper/AGENTS.md): privileged staging helper runtime
- [`src/cli/AGENTS.md`](src/cli/AGENTS.md): user-facing commands
- [`src/compare/AGENTS.md`](src/compare/AGENTS.md): compare process and privilege drop
- [`src/config/AGENTS.md`](src/config/AGENTS.md): config schema, parsing, secure writes
- [`src/storage/AGENTS.md`](src/storage/AGENTS.md): user-model storage/codec/readiness
- [`src/vision/AGENTS.md`](src/vision/AGENTS.md): camera and face pipeline
- [`tests/AGENTS.md`](tests/AGENTS.md): test ownership and support headers

## Package Invariants

- `howdy` is the only installed user-facing CLI. Do not add standalone command executables.
- Keep privileged helpers under `<libexecdir>/howdy`; this includes `howdy-compare` and setuid
  `howdy-auth-helper`.
- Use typed `RuntimeConfig`, `VideoConfig`, and `FaceConfig` at runtime boundaries.
- Reuse `support/fd_io.hpp`, `support/atomic_files.hpp`, and `support/file_security.hpp` instead of
  duplicating bounded I/O, staged-file, or secure-path logic.
- Use shared model readiness/integrity checks before inference or authentication.
- Keep OpenCV include directories marked as system includes through `howdy_opencv`.
- Shared auth-helper output keys live in `protocol/auth_helper_protocol.hpp`; reject malformed,
  duplicate, unknown, or incomplete output.

## CMake and Tests

`howdy/CMakeLists.txt` owns production/test source membership. Keep every new translation unit in
exactly one intended target and avoid adding `howdy/src` as a global include root just to reach
private headers.

Run focused tests while editing, then both Release and Debug full suites before a broad maintenance
change is considered complete.
