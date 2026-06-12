# CLI Knowledge Base

**Updated:** 2026-06-11

## Scope

Native C++ CLI commands for model management, config editing, camera testing, and snapshot generation.

## Where To Look

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

## Conventions

- Keep config edits atomic and secure.
- Reuse `common/invoking_user*.hpp` for invoking-user helpers.
- Reuse `common/model_file.hpp` for model integrity checks.
- For runtime commands such as add, test, and snapshot, prefer typed `RuntimeConfig` fields over raw `ConfigReader` access.
- Keep command behavior aligned with installed `/etc/howdy` layout.
- Prefer shared helpers in `howdy/src/config`, `howdy/src/storage`, and `howdy/include/common`.
