# CLI KNOWLEDGE BASE

**Generated:** 2026-05-08
**Commit:** bc03695

## OVERVIEW

CLI subcommands for face model management, auth toggling, config editing, and camera testing.

## WHERE TO LOOK

| Command     | File         | Role                                    |
| ----------- | ------------ | --------------------------------------- |
| Add face    | `add.py`     | Capture and encode face model           |
| Clear all   | `clear.py`   | Delete all models for user              |
| Edit config | `config.py`  | Open config.ini in $EDITOR              |
| Toggle auth | `disable.py` | Enable/disable howdy                    |
| List models | `list.py`    | Show user's face models with IDs        |
| Remove one  | `remove.py`  | Delete model by ID                      |
| Set config  | `set.py`     | Update config value                     |
| Snapshot    | `snap.py`    | Generate diagnostic image               |
| Camera test | `test.py`    | Live preview window with face detection |

## CONVENTIONS

- **Args**: `builtins.howdy_args` global (`.arguments[]`, `.plain`, `.y` flags)
- **User**: `builtins.howdy_user` for current username
- **i18n**: `from i18n import _` for translations
- **Paths**: Use `paths_factory` module, not hardcoded paths
- **Config write**: Atomic tempfile + `os.replace()` or `shutil.move()`
- **Future imports**: `from __future__ import annotations` in all files

## ANTI-PATTERNS

- **NEVER** `fileinput.input()` for config editing (race condition)
- **NEVER** hardcode paths; use `paths_factory`
