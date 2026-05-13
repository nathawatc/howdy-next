# CLI KNOWLEDGE BASE

**Generated:** 2026-05-13

## OVERVIEW

Native C++ CLI subcommands for model management, config editing, snapshot generation, and camera testing.

## WHERE TO LOOK

| Command      | File                              | Role                               |
| ------------ | --------------------------------- | ---------------------------------- |
| Add face     | `howdy/src/cli/add.cpp`   | Capture and encode face model      |
| Clear all    | `howdy/src/cli/clear.cpp` | Delete all models for user         |
| Edit config  | `howdy/src/cli/config.cpp`| Open `config.ini` in `$EDITOR`     |
| Toggle auth  | `howdy/src/cli/disable.cpp` | Enable/disable howdy             |
| List models  | `howdy/src/cli/list.cpp`  | Show user's face models with IDs   |
| Remove one   | `howdy/src/cli/remove.cpp`| Delete model by ID                 |
| Set config   | `howdy/src/cli/set.cpp`   | Update config value                |
| Download ONNX | `howdy/src/cli/download_models.cpp` | Fetch face models        |
| Snapshot     | `howdy/src/cli/snapshot.cpp` | Generate diagnostic image       |
| Camera test  | `howdy/src/cli/test.cpp`   | Live preview window with compare   |

## CONVENTIONS

- Keep config edits atomic.
- Keep command behavior aligned with the installed Arch config path.
- Prefer shared native helpers in `howdy/src/config` and `howdy/src/storage`.
