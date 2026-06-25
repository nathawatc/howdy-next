# Howdy Source Guidelines

**Updated:** 2026-06-25

## Scope

Shared runtime code under `howdy/src/` and headers under `howdy/include/`.

## Conventions

- Prefer shared helpers for storage, config, readiness, model checks, and runtime staging.
- Do not reintroduce duplicated file-validation or config-loading logic in feature code.
- Runtime consumers should use typed `RuntimeConfig`, `VideoConfig`, and `FaceConfig`
  values instead of direct `ConfigReader` key lookups.
- `RuntimeConfigLoadResult::config` is optional; check status and `config.has_value()` before dereferencing.
- Keep runtime behavior fail-closed on validation or ownership errors.
- Use C++23 idioms when editing shared runtime code.
- Use centralized `read_fd_to_string_bounded()` and `write_fd_from_buffer()`
  from `common/fd_io.hpp` for bounded FD I/O.
- Enrollment capture logic lives in `cli/enrollment_capture.hpp`
  (template `capture_enrollment_sample()`); classify failures
  with `classify_enrollment_capture_failure()`.
- Auth helper protocol keys are shared via `common/auth_helper_protocol.hpp`.
- Build requires `opencv4` with `include_type: 'system'` for system OpenCV.
