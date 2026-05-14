# CLI KNOWLEDGE BASE

**Generated:** 2026-05-14

## OVERVIEW

Native C++ CLI commands for model management, config editing, camera testing,
and snapshot generation.

## WHERE TO LOOK

| Command       | File                                | Role                               |
| ------------- | ----------------------------------- | ---------------------------------- |
| Add face      | `howdy/src/cli/add.cpp`             | Capture and encode a user model    |
| Clear all     | `howdy/src/cli/clear.cpp`           | Delete all user models             |
| Edit config   | `howdy/src/cli/config.cpp`          | Open config in `$EDITOR` safely    |
| Toggle auth   | `howdy/src/cli/disable.cpp`         | Enable or disable auth             |
| List models   | `howdy/src/cli/list.cpp`            | Show user model IDs                |
| Remove one    | `howdy/src/cli/remove.cpp`          | Delete one model by ID             |
| Set config    | `howdy/src/cli/set.cpp`             | Update one config value atomically |
| Download ONNX | `howdy/src/cli/download_models.cpp` | Fetch packaged face models         |
| Snapshot      | `howdy/src/cli/snapshot.cpp`        | Generate a diagnostic frame        |
| Camera test   | `howdy/src/cli/test.cpp`            | Live preview and compare flow      |

## CONVENTIONS

- Keep config edits atomic and secure.
- `config.cpp` and `test.cpp` share invoking-user helpers from `common/invoking_user*.hpp`.
- Model corruption checks are shared in `common/model_file.hpp`.
- Keep command behavior aligned with the installed `/etc/howdy` layout.
- Prefer shared helpers in `howdy/src/config`, `howdy/src/storage`, and `howdy/include/common`.
