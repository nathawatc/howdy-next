# CLI Knowledge Base

**Updated:** 2026-07-01

## Scope

Native C++ CLI commands for model management, config editing, camera testing, and snapshot generation.

## Where to Look

| Command       | File                                | Role                               |
| ------------- | ----------------------------------- | ---------------------------------- |
| Add face      | `howdy/src/cli/add.cpp`             | Capture and encode user model      |
| Clear all     | `howdy/src/cli/clear.cpp`           | Delete all user models             |
| Edit config   | `howdy/src/cli/config.cpp`          | Open config in `$EDITOR` safely    |
| Toggle auth   | `howdy/src/cli/disable.cpp`         | Enable or disable auth             |
| List models   | `howdy/src/cli/list.cpp`            | Show user model IDs                |
| Remove one    | `howdy/src/cli/remove.cpp`          | Delete one model by ID             |
| Set config    | `howdy/src/cli/set.cpp`             | Update one config value atomically |
| Download ONNX | `howdy/src/cli/download_models.cpp` | Fetch packaged face models         |
| Snapshot      | `howdy/src/cli/snapshot.cpp`        | Generate diagnostic frame          |
| Camera test   | `howdy/src/cli/test.cpp`            | Live preview and compare flow      |

## Internal Headers (Testability)

CLI commands are refactored for testability via dependency-injection structs:

| Header                                     | Exposes                                                                                                               |
| ------------------------------------------ | --------------------------------------------------------------------------------------------------------------------- |
| `include/cli/add_internal.hpp`             | `AddDependencies`, `add_main_with_dependencies()`                                                                     |
| `include/cli/list_internal.hpp`            | `ListDependencies`, `list_main_with_dependencies()`                                                                   |
| `include/cli/remove_internal.hpp`          | `RemoveDependencies`, `remove_main_with_dependencies()`                                                               |
| `include/cli/set_internal.hpp`             | `SetDependencies`, `set_main_with_dependencies()`                                                                     |
| `include/cli/test_cli_internal.hpp`        | `TestDependencies`, `test_main_with_dependencies()`, `run_preview_preflight()`, `has_graphical_display_environment()` |
| `include/cli/snapshot_internal.hpp`        | `SnapshotDependencies`, `SnapshotWriterDependencies`, `snapshot_main_with_dependencies()`, `write_snapshot_at_path()` |
| `include/cli/enrollment_capture.hpp`       | `capture_enrollment_sample()` template, `EnrollmentCaptureResult`, `classify_enrollment_capture_failure()`            |
| `include/cli/download_models_internal.hpp` | Download models internals                                                                                             |

## Conventions

- Keep config edits atomic and secure.
- Reuse `common/invoking_user*.hpp` for invoking-user helpers.
- Reuse `common/model_file.hpp` for model integrity checks.
- For runtime commands such as add, test, and snapshot, prefer typed
  `RuntimeConfig` fields over raw `ConfigReader` access.
- Keep command behavior aligned with installed `/etc/howdy` layout.
- Prefer shared helpers in `howdy/src/config`, `howdy/src/storage`, and `howdy/include/common`.
- Add, test, and snapshot commands are split into production `*_main.cpp`
  plus shared implementation for testability; avoid duplicating the
  dependency-injection seam.
- List, remove, and set public wrappers retain production behavior while injected
  `*_main_with_dependencies()` runners support tests.
- Snapshot writer validates BGR frame batches and atomically installs output.
- Capture failure diagnostics use `classify_enrollment_capture_failure()`
  for granular error messages (black frames, too dark, no face, etc.).
