# CLI KNOWLEDGE BASE

**Generated:** 2026-05-13

## OVERVIEW

Native C++ CLI subcommands for model management, config editing, snapshot generation, and camera testing.

## WHERE TO LOOK

| Command      | File                              | Role                               |
| ------------ | --------------------------------- | ---------------------------------- |
| Add face     | `howdy/src/lib/cpp/cli/add.cpp`   | Capture and encode face model      |
| Clear all    | `howdy/src/lib/cpp/cli/clear.cpp` | Delete all models for user         |
| Edit config  | `howdy/src/lib/cpp/cli/config.cpp`| Open `config.ini` in `$EDITOR`     |
| Toggle auth  | `howdy/src/lib/cpp/cli/disable.cpp` | Enable/disable howdy             |
| List models  | `howdy/src/lib/cpp/cli/list.cpp`  | Show user's face models with IDs   |
| Remove one   | `howdy/src/lib/cpp/cli/remove.cpp`| Delete model by ID                 |
| Set config   | `howdy/src/lib/cpp/cli/set.cpp`   | Update config value                |
| Download ONNX | `howdy/src/lib/cpp/cli/download_models.cpp` | Fetch face models        |
| Snapshot     | `howdy/src/lib/cpp/cli/snapshot.cpp` | Generate diagnostic image       |
| Camera test  | `howdy/src/lib/cpp/cli/test.cpp`   | Live preview window with compare   |

## CONVENTIONS

- Keep config edits atomic.
- Keep command behavior aligned with the installed Arch config path.
- Prefer shared native helpers in `howdy/src/lib/cpp/config` and `howdy/src/lib/cpp/storage`.
